#!/usr/bin/env bash
# End-to-end smoke test for jx-engine using the offline tiny test model.
#
# Usage: scripts/smoke-test.sh [path-to-jx-engine-binary]
#
# Requires: python3 + numpy (to generate the test model), curl.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="${1:-build/jx-engine}"
MODEL="models-test/tiny-llama-random.gguf"
PORT="${JX_SMOKE_PORT:-18190}"
EMB_PORT=$((PORT + 1))
NP_PORT=$((PORT + 2))
BASE="http://127.0.0.1:$PORT"
EMB_BASE="http://127.0.0.1:$EMB_PORT"
NP_BASE="http://127.0.0.1:$NP_PORT"

PASS=0
FAIL=0
PIDS=()

cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
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

json_has() { # url-args... jq-ish python expression reading parsed json as d
    local expr="$1"; shift
    curl -sf "$@" | python3 -c "import json,sys; d=json.load(sys.stdin); assert $expr, d"
}

[ -x "$BIN" ] || { echo "error: binary '$BIN' not found (build first)"; exit 1; }
[ -f "$MODEL" ] || python3 scripts/make-tiny-model.py "$MODEL"

echo "== CLI"
check "--version" bash -c "'$BIN' --version | grep -q jx-engine"
check "--help lists --cache-reuse" bash -c "'$BIN' --help | grep -q -- --cache-reuse"
check "--help lists --parallel" bash -c "'$BIN' --help | grep -q -- --parallel"
check "--help lists --context-shift" bash -c "'$BIN' --help | grep -q -- --context-shift"

echo "== starting generation instance on :$PORT"
"$BIN" -m "$MODEL" --port "$PORT" -c 512 --alias tiny-test > /dev/null 2>&1 &
PIDS+=($!)
echo "== starting embedding instance on :$EMB_PORT"
"$BIN" -m "$MODEL" --port "$EMB_PORT" --embedding --pooling mean > /dev/null 2>&1 &
PIDS+=($!)
echo "== starting 2-slot context-shift instance on :$NP_PORT"
"$BIN" -m "$MODEL" --port "$NP_PORT" -c 512 -np 2 --context-shift --alias tiny-np2 > /dev/null 2>&1 &
PIDS+=($!)

for i in $(seq 1 100); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && curl -sf "$EMB_BASE/health" > /dev/null 2>&1 \
        && curl -sf "$NP_BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done

echo "== endpoints"
check "GET /health" curl -sf "$BASE/health"
check "GET /props reports n_ctx" \
    json_has 'd["n_ctx"] == 512 and d["default_generation_settings"]["n_ctx"] == 512 and "chat_template" in d' "$BASE/props"
check "GET /v1/models echoes alias" \
    json_has 'd["data"][0]["id"] == "tiny-test"' "$BASE/v1/models"
check "POST /tokenize" \
    json_has 'len(d["tokens"]) > 0' -X POST "$BASE/tokenize" -d '{"content":"Hello world"}'
check "POST /detokenize round trip" \
    json_has '"Hello" in d["content"]' -X POST "$BASE/detokenize" -d '{"tokens":[15043,3186]}'
check "POST /apply-template" \
    json_has '"user: Hi" in d["prompt"]' -X POST "$BASE/apply-template" -d '{"messages":[{"role":"user","content":"Hi"}]}'
check "POST /v1/completions" \
    json_has 'd["usage"]["completion_tokens"] == 8 and d["model"] == "tiny-test"' \
    -X POST "$BASE/v1/completions" -d '{"prompt":"Once upon a time","max_tokens":8}'
check "POST /v1/chat/completions" \
    json_has 'd["choices"][0]["message"]["role"] == "assistant" and d["choices"][0]["finish_reason"] == "length"' \
    -X POST "$BASE/v1/chat/completions" -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":8}'
check "chat timings present" \
    json_has '"prompt_ms" in d["timings"] and "predicted_per_second" in d["timings"]' \
    -X POST "$BASE/v1/chat/completions" -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":4}'
check "json_schema constrained output is valid JSON" \
    bash -c "curl -sf -X POST '$BASE/v1/chat/completions' -d '{\"messages\":[{\"role\":\"user\",\"content\":\"person\"}],\"max_tokens\":40,\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"schema\":{\"type\":\"object\",\"properties\":{\"age\":{\"type\":\"integer\"}},\"required\":[\"age\"],\"additionalProperties\":false}}}}' | python3 -c 'import json,sys; d=json.load(sys.stdin); json.loads(d[\"choices\"][0][\"message\"][\"content\"])'"
check "KV prefix reuse reports cache_n" bash -c "
    curl -sf -X POST '$BASE/v1/completions' -d '{\"prompt\":\"The quick brown fox jumps over\",\"max_tokens\":4}' > /dev/null
    curl -sf -X POST '$BASE/v1/completions' -d '{\"prompt\":\"The quick brown fox jumps over\",\"max_tokens\":4}' \
      | python3 -c 'import json,sys; assert json.load(sys.stdin)[\"timings\"][\"cache_n\"] > 0'"

echo "== streaming"
STREAM=$(curl -sfN -X POST "$BASE/v1/chat/completions" -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":5,"stream":true}')
check "SSE data frames"     bash -c "grep -q '^data: {' <<< '$STREAM'"
check "SSE [DONE] frame"    bash -c "grep -q '^data: \[DONE\]' <<< '$STREAM'"
check "SSE final usage"     bash -c "grep -q '\"usage\"' <<< '$STREAM'"
check "SSE finish_reason"   bash -c "grep -q '\"finish_reason\":\"length\"' <<< '$STREAM'"

echo "== embeddings instance"
check "POST /v1/embeddings" \
    json_has 'len(d["data"]) == 2 and len(d["data"][0]["embedding"]) == 64 and d["usage"]["prompt_tokens"] > 0' \
    -X POST "$EMB_BASE/v1/embeddings" -d '{"input":["hello world","goodbye"]}'
check "generation rejected in embedding mode (501)" bash -c "
    code=\$(curl -s -o /dev/null -w '%{http_code}' -X POST '$EMB_BASE/v1/chat/completions' -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}]}')
    [ \"\$code\" = 501 ]"
check "embeddings rejected in generation mode (501)" bash -c "
    code=\$(curl -s -o /dev/null -w '%{http_code}' -X POST '$BASE/v1/embeddings' -d '{\"input\":\"x\"}')
    [ \"\$code\" = 501 ]"

echo "== parallel slots (-np 2) instance"
check "chat completion on the 2-slot instance" \
    json_has 'd["choices"][0]["message"]["role"] == "assistant" and d["usage"]["completion_tokens"] == 8' \
    -X POST "$NP_BASE/v1/chat/completions" -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":8}'
check "per-slot KV prefix reuse reports cache_n" bash -c "
    curl -sf -X POST '$NP_BASE/v1/completions' -d '{\"prompt\":\"A slot-affine prompt for reuse\",\"max_tokens\":4}' > /dev/null
    curl -sf -X POST '$NP_BASE/v1/completions' -d '{\"prompt\":\"A slot-affine prompt for reuse\",\"max_tokens\":4}' \
      | python3 -c 'import json,sys; assert json.load(sys.stdin)[\"timings\"][\"cache_n\"] > 0'"

# two streaming requests issued at the same time must both complete with
# correct usage totals; whether they actually overlapped is reported but not
# asserted (a loaded CI box can run them back to back)
PAR_LOG="$(mktemp)"
if python3 - "$NP_BASE" > "$PAR_LOG" 2>&1 <<'PYEOF'
import json, sys, threading, time, urllib.request

base = sys.argv[1]
n_predict = 200
out = {}

def run(tag):
    body = json.dumps({"prompt": "Once upon a time in " + tag, "max_tokens": n_predict,
                       "temperature": 0, "stream": True}).encode()
    req = urllib.request.Request(base + "/v1/completions", data=body,
                                 headers={"Content-Type": "application/json"})
    first = None
    usage = None
    with urllib.request.urlopen(req) as r:
        for raw in r:
            line = raw.decode("utf-8", "replace").strip()
            if not line.startswith("data: "):
                continue
            payload = line[6:]
            if payload == "[DONE]":
                break
            frame = json.loads(payload)
            if first is None:
                first = time.time()
            if frame.get("usage"):
                usage = frame["usage"]
    out[tag] = (first, time.time(), usage)

threads = [threading.Thread(target=run, args=(t,)) for t in ("A", "B")]
for t in threads:
    t.start()
for t in threads:
    t.join()

for tag in ("A", "B"):
    first, end, usage = out[tag]
    assert usage is not None, ("no usage frame", tag)
    assert usage["completion_tokens"] == n_predict, (tag, usage)
    assert usage["total_tokens"] == usage["prompt_tokens"] + usage["completion_tokens"], (tag, usage)

a, b = out["A"], out["B"]
overlapped = a[0] < b[1] and b[0] < a[1]
print("info - concurrent streams %s" % ("overlapped" if overlapped else "ran back to back"))
PYEOF
then PAR_RC=0; else PAR_RC=1; fi
grep '^info' "$PAR_LOG" || true
check "two concurrent streams both complete correctly" test "$PAR_RC" = 0
rm -f "$PAR_LOG"

echo "== context shift"
check "without --context-shift generation stops at the context limit" \
    json_has 'd["choices"][0]["finish_reason"] == "length" and d["usage"]["completion_tokens"] < 700 and d["usage"]["total_tokens"] == 512' \
    -X POST "$BASE/v1/completions" -d '{"prompt":"Once upon a time","max_tokens":700,"temperature":0}'
# per-slot context on the -np 2 instance is 512/2 = 256, so a 5-token prompt
# leaves 251 tokens of headroom; --context-shift must generate well past it
check "with --context-shift generation runs past the per-slot context" \
    json_has 'd["usage"]["completion_tokens"] == 400 and d["usage"]["prompt_tokens"] == 5 and d["choices"][0]["finish_reason"] == "length"' \
    -X POST "$NP_BASE/v1/completions" -d '{"prompt":"Once upon a time","max_tokens":400,"temperature":0}'
check "the shifted instance still serves normal requests afterwards" \
    json_has 'd["usage"]["completion_tokens"] == 6' \
    -X POST "$NP_BASE/v1/completions" -d '{"prompt":"Hello again","max_tokens":6,"temperature":0}'

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
