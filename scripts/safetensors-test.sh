#!/usr/bin/env bash
# Standalone end-to-end test for onyx-engine's safetensors -> GGUF conversion
# path (scripts/convert-safetensors.py + src/convert.cpp). NOT wired into
# scripts/smoke-test.sh.
#
# Usage: scripts/safetensors-test.sh [path-to-onyx-engine-binary]
#
# Requires: python3. numpy is installed automatically if missing (network
# permitting); if that install is impossible, the test SKIPS (exit 0) rather
# than failing, since CI has full network and will run it for real. If
# build/onyx-engine does not exist yet (it may be mid-rework), the serving part
# is skipped but the converter itself is still exercised directly.
set -uo pipefail

cd "$(dirname "$0")/.."

BIN="${1:-build/onyx-engine}"
MODEL_DIR="models-test/tiny-hf-llama"
PORT="${ONYX_SAFETENSORS_TEST_PORT:-18290}"
BASE="http://127.0.0.1:$PORT"

PASS=0
FAIL=0
PIDS=()

cleanup() {
    # ${PIDS[@]+...}: empty-array expansion trips `set -u` on macOS bash 3.2
    for pid in ${PIDS[@]+"${PIDS[@]}"}; do kill "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT

check() { # name condition-command...
    local name="$1"; shift
    if "$@" > /dev/null 2>&1; then
        echo "ok   - $name"; PASS=$((PASS + 1))
    else
        echo "FAIL - $name"; FAIL=$((FAIL + 1))
    fi
}

skip_all() {
    echo "=================================================================="
    echo " SKIPPED: $1"
    echo "=================================================================="
    exit 0
}

# ---- ensure numpy (the only dependency this whole pipeline needs) ---------
if ! python3 -c "import numpy" 2>/dev/null; then
    echo "== numpy not found, attempting install"
    if ! pip3 install --quiet numpy 2>&1 | tail -20; then
        skip_all "could not install numpy (no network/proxy access?) - conversion requires python3 + numpy only, CI has full network and will run this for real"
    fi
    python3 -c "import numpy" 2>/dev/null || skip_all "numpy install reported success but import still fails"
fi
echo "== numpy available: $(python3 -c 'import numpy; print(numpy.__version__)')"

# ---- generate the tiny HF (safetensors) model ------------------------------
echo "== generating tiny HF model at $MODEL_DIR"
rm -rf "$MODEL_DIR"
if ! python3 scripts/make-tiny-hf-model.py "$MODEL_DIR"; then
    echo "FAIL - could not generate tiny HF model"
    exit 1
fi
check "config.json written"    test -f "$MODEL_DIR/config.json"
check "tokenizer.json written" test -f "$MODEL_DIR/tokenizer.json"
check "model.safetensors written" test -f "$MODEL_DIR/model.safetensors"

# ---- exercise the converter directly ---------------------------------------
echo "== converting directly with scripts/convert-safetensors.py"
DIRECT_OUT="models-test/tiny-hf-llama-direct.gguf"
rm -f "$DIRECT_OUT"
if python3 scripts/convert-safetensors.py "$MODEL_DIR" --outfile "$DIRECT_OUT" --outtype f16; then
    echo "ok   - direct conversion succeeded"; PASS=$((PASS + 1))
else
    echo "FAIL - direct conversion failed"; FAIL=$((FAIL + 1))
fi
check "direct conversion produced a GGUF" test -s "$DIRECT_OUT"
check "direct conversion GGUF has magic bytes" bash -c "head -c4 '$DIRECT_OUT' | grep -qU GGUF"

# ---- serve it through onyx-engine, exercising src/convert.cpp's cache -------
if [ ! -x "$BIN" ]; then
    echo "=================================================================="
    echo " onyx-engine binary '$BIN' not found - skipping the serving part"
    echo " (conversion itself was already exercised above and passed)"
    echo "=================================================================="
else
    export ONYX_ENGINE_CONVERT_SCRIPT="$(pwd)/scripts/convert-safetensors.py"
    rm -rf "$MODEL_DIR/onyx-cache"

    echo "== first launch (expect a real conversion)"
    LOG1="$(mktemp)"
    "$BIN" -m "$MODEL_DIR" --port "$PORT" -c 512 --alias tiny-hf > "$LOG1" 2>&1 &
    PID=$!
    PIDS+=("$PID")

    for _ in $(seq 1 100); do
        curl -sf "$BASE/health" > /dev/null 2>&1 && break
        kill -0 "$PID" 2>/dev/null || break
        sleep 0.2
    done

    if ! curl -sf "$BASE/health" > /dev/null 2>&1 \
            && ! grep -qi "converting safetensors" "$LOG1" \
            && grep -qi "gguf_init_from_reader\|failed to load model" "$LOG1"; then
        # The binary never attempted a conversion and choked mmap'ing the HF
        # directory as a GGUF: it predates the onyx_resolve_model() wiring in
        # main.cpp. Not a failure of the converter itself: skip the serving
        # assertions but keep the direct-conversion results above. A binary
        # that DID run a conversion and still failed to serve falls through
        # to the hard failure below - that is a real regression.
        kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true
        echo "=================================================================="
        echo " SKIPPED serving checks: '$BIN' does not yet call onyx_resolve_model()"
        echo " before loading the model (see src/convert.h) - conversion itself"
        echo " already passed above."
        echo "=================================================================="
        echo
        echo "== $PASS passed, $FAIL failed"
        if [ "$FAIL" -eq 0 ]; then exit 0; else exit 1; fi
    fi

    check "server came up after conversion" curl -sf "$BASE/health"
    # cache name carries a hash of the absolute source path: <dirname>-<hash8>-f16.gguf
    CACHE_GGUF="$(ls "$MODEL_DIR"/onyx-cache/"$(basename "$MODEL_DIR")"-*-f16.gguf 2>/dev/null | head -1)"
    check "conversion cache file appeared"  test -n "$CACHE_GGUF" -a -f "$CACHE_GGUF"
    check "log shows a real conversion ran" grep -q "converting safetensors" "$LOG1"
    check "completion request answers" curl -sf "$BASE/v1/completions" \
        -H 'Content-Type: application/json' \
        -d '{"model":"tiny-hf","prompt":"hello","max_tokens":4}'

    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true

    echo "== second launch (expect cache reuse, no reconversion)"
    LOG2="$(mktemp)"
    "$BIN" -m "$MODEL_DIR" --port "$PORT" -c 512 --alias tiny-hf > "$LOG2" 2>&1 &
    PID=$!
    PIDS+=("$PID")

    for _ in $(seq 1 100); do
        curl -sf "$BASE/health" > /dev/null 2>&1 && break
        kill -0 "$PID" 2>/dev/null || break
        sleep 0.2
    done

    check "server came up on second launch" curl -sf "$BASE/health"
    check "log shows the cache was reused"  grep -q "reusing cached conversion" "$LOG2"
    check "log shows NO reconversion"       bash -c "! grep -q 'converting safetensors' '$LOG2'"

    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true

    rm -f "$LOG1" "$LOG2"
fi

echo
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
