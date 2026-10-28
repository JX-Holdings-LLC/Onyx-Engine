#!/usr/bin/env bash
# End-to-end smoke test for onyx-engine using the offline tiny test model.
#
# Usage: scripts/smoke-test.sh [path-to-onyx-engine-binary]
#
# Requires: python3 (or python) + numpy (to generate the test model), curl,
# and Bash. Runs on Linux and macOS, and on Windows under Git Bash (the CI
# windows leg), where the binary is build/Release/onyx-engine.exe.
set -euo pipefail

cd "$(dirname "$0")/.."

# Python decodes stdin with the locale's code page on Windows (cp1252), which
# garbles the UTF-8 JSON piped into the checks; UTF-8 mode fixes that.
export PYTHONUTF8=1

# Windows Python installs (and venvs) provide python.exe but no python3.exe.
# Exported so the `bash -c` checks below inherit it.
if ! command -v python3 > /dev/null 2>&1 && command -v python > /dev/null 2>&1; then
    python3() { python "$@"; }
    export -f python3
fi
# A path as the native binary sees it: C:/... under Git Bash, unchanged elsewhere.
native_path() { (cd "$(dirname "$1")" && printf '%s/%s' "$(pwd -W 2> /dev/null || pwd)" "$(basename "$1")"); }

if [ $# -ge 1 ]; then
    BIN="$1"
else
    BIN=build/onyx-engine
    for cand in build/onyx-engine build/Release/onyx-engine.exe build/onyx-engine.exe; do
        [ -f "$cand" ] && { BIN="$cand"; break; }
    done
fi
MODEL="models-test/tiny-llama-random.gguf"
MMPROJ="models-test/tiny-mmproj-random.gguf"
DEC_MODEL="models-test/tiny-llama-decision.gguf"
PORT="${ONYX_SMOKE_PORT:-18190}"
EMB_PORT=$((PORT + 1))
NP_PORT=$((PORT + 2))
MM_PORT=$((PORT + 3))
RB_PORT=$((PORT + 4))
RK_PORT=$((PORT + 5))
DEC_PORT=$((PORT + 6))
OPS_PORT=$((PORT + 7))
TT_PORT=$((PORT + 8))
LOAD_PORT=$((PORT + 9))
MEM_PORT=$((PORT + 10))
BASE="http://127.0.0.1:$PORT"
EMB_BASE="http://127.0.0.1:$EMB_PORT"
NP_BASE="http://127.0.0.1:$NP_PORT"
MM_BASE="http://127.0.0.1:$MM_PORT"
RB_BASE="http://127.0.0.1:$RB_PORT"
RK_BASE="http://127.0.0.1:$RK_PORT"
DEC_BASE="http://127.0.0.1:$DEC_PORT"
OPS_BASE="http://127.0.0.1:$OPS_PORT"
TT_BASE="http://127.0.0.1:$TT_PORT"
LOAD_BASE="http://127.0.0.1:$LOAD_PORT"
MEM_BASE="http://127.0.0.1:$MEM_PORT"
OPS_KEY="sk-smoke"

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
    local output
    if output=$("$@" 2>&1); then
        echo "ok   - $name"; PASS=$((PASS + 1))
    else
        echo "FAIL - $name"; FAIL=$((FAIL + 1))
        [ -z "$output" ] || printf '%s\n' "$output"
        for log in models-test/onyx-*.log; do
            [ -f "$log" ] && { echo "== $log"; tail -30 "$log"; }
        done
    fi
}

json_has() { # url-args... jq-ish python expression reading parsed json as d
    local expr="$1"; shift
    curl -sf "$@" | python3 -c "import json,sys; d=json.load(sys.stdin); assert $expr, d"
}

http_status() { # expected-status curl-args...
    local expected="$1"; shift
    [ "$(curl -s -o /dev/null -w '%{http_code}' "$@")" = "$expected" ]
}

[ -x "$BIN" ] || { echo "error: binary '$BIN' not found (build first)"; exit 1; }
[ -f "$MODEL" ]  || python3 scripts/make-tiny-model.py "$MODEL"
[ -f "$MMPROJ" ] || python3 scripts/make-tiny-mmproj.py "$MMPROJ"
[ -f "$DEC_MODEL" ] || python3 scripts/make-tiny-model.py --decision "$DEC_MODEL"

echo "== CLI"
check "--version" bash -c "'$BIN' --version | grep -q onyx-engine"
check "--help lists --cache-reuse" bash -c "'$BIN' --help | grep -q -- --cache-reuse"
check "--help lists --parallel" bash -c "'$BIN' --help | grep -q -- --parallel"
check "--help lists --context-shift" bash -c "'$BIN' --help | grep -q -- --context-shift"
check "--help lists --mmproj" bash -c "'$BIN' --help | grep -q -- --mmproj"
check "--help lists --no-webui" bash -c "'$BIN' --help | grep -q -- --no-webui"
for flag in --max-queue --tick-tokens --metrics --allow-unauthenticated --queue-timeout \
            --cache-type-k --cache-type-v --cpu-moe --n-cpu-moe --override-tensor; do
    check "--help lists $flag" bash -c "'$BIN' --help | grep -q -- $flag"
done
check "a non-loopback --host without --api-key is refused" bash -c "
    ! '$BIN' -m '$MODEL' --host 0.0.0.0 --port $LOAD_PORT > models-test/refuse.out 2>&1 &&
    grep -q -- '--allow-unauthenticated' models-test/refuse.out"
check "--port -1 is refused" bash -c "! '$BIN' -m '$MODEL' --port -1 2> /dev/null"
check "a quantized --cache-type-v with -fa off is refused" bash -c "
    ! '$BIN' -m '$MODEL' -ctv q8_0 -fa off --port $LOAD_PORT > models-test/refuse.out 2>&1 &&
    grep -q 'needs flash attention' models-test/refuse.out"
# the tiny model's 16-wide heads do not divide q8_0's 32-wide blocks, so the
# type reaching libllama shows as a clear load failure
check "a KV cache type the model cannot hold fails the load and names the types" bash -c "
    ! '$BIN' -m '$MODEL' -ctk q8_0 -ctv q8_0 --port $LOAD_PORT > models-test/refuse.out 2>&1 &&
    grep -q 'cache types q8_0/q8_0' models-test/refuse.out"
check "-ot with an unknown buffer type fails the load and lists the known ones" bash -c "
    ! '$BIN' -m '$MODEL' -ot 'exps=NO_SUCH_BUFFER' --port $LOAD_PORT > models-test/refuse.out 2>&1 &&
    grep -q 'unknown buffer type .NO_SUCH_BUFFER. (available: .*CPU' models-test/refuse.out"

echo "== starting generation instance on :$PORT"
"$BIN" -m "$MODEL" --port "$PORT" -c 512 --alias tiny-test > models-test/onyx-generation.log 2>&1 &
PIDS+=($!)
echo "== starting embedding instance on :$EMB_PORT"
"$BIN" -m "$MODEL" --port "$EMB_PORT" --embedding --pooling mean > models-test/onyx-embedding.log 2>&1 &
PIDS+=($!)
echo "== starting rank-pooling embedding instance on :$RK_PORT"
"$BIN" -m "$MODEL" --port "$RK_PORT" --embedding --pooling rank > models-test/onyx-rank.log 2>&1 &
PIDS+=($!)
echo "== starting 2-slot context-shift instance on :$NP_PORT"
"$BIN" -m "$MODEL" --port "$NP_PORT" -c 512 -np 2 --context-shift --alias tiny-np2 > models-test/onyx-parallel.log 2>&1 &
PIDS+=($!)
echo "== starting multimodal instance on :$MM_PORT"
"$BIN" -m "$MODEL" --port "$MM_PORT" -c 512 --mmproj "$MMPROJ" --alias tiny-mm > models-test/onyx-multimodal.log 2>&1 &
PIDS+=($!)

echo "== starting operations instance on :$OPS_PORT (--metrics, --max-queue 1, --api-key)"
"$BIN" -m "$MODEL" --port "$OPS_PORT" -c 512 -np 1 --max-queue 1 --metrics --api-key "$OPS_KEY" \
    --alias tiny-ops > models-test/onyx-ops.log 2>&1 &
PIDS+=($!)
echo "== starting 2-slot tick-budget instance on :$TT_PORT (--tick-tokens 16)"
"$BIN" -m "$MODEL" --port "$TT_PORT" -c 1024 -np 2 --tick-tokens 16 --alias tiny-tt > models-test/onyx-tick.log 2>&1 &
PIDS+=($!)

echo "== starting memory-flags instance on :$MEM_PORT (-ctk f32 -ctv bf16, MoE/-ot overrides, --queue-timeout 1)"
"$BIN" -m "$MODEL" --port "$MEM_PORT" -c 512 -np 1 -ctk f32 -ctv bf16 --cpu-moe -ncmoe 2 -ot 'token_embd=CPU' \
    --queue-timeout 1 --metrics --alias tiny-mem > models-test/onyx-memory.log 2>&1 &
PIDS+=($!)

echo "== starting decision-model instance on :$DEC_PORT"
"$BIN" -m "$DEC_MODEL" --port "$DEC_PORT" -c 512 --alias tiny-decision > models-test/onyx-decision.log 2>&1 &
PIDS+=($!)

# a deepseek-style template whose generation prompt itself ends inside the
# thinking block ("...assistant:<think>"), used by the --reasoning-budget
# tests below; written with printf (not a heredoc) so it carries no trailing
# newline, which would otherwise land inside the rendered prompt.
RB_TEMPLATE="$(mktemp)"
printf '{%% for m in messages %%}{{ m["role"] }}: {{ m["content"] }}\n{%% endfor %%}{%% if add_generation_prompt %%}assistant:<think>{%% endif %%}' > "$RB_TEMPLATE"
echo "== starting reasoning-budget instance on :$RB_PORT"
"$BIN" -m "$MODEL" --port "$RB_PORT" -c 512 --alias tiny-rb \
    --chat-template-file "$RB_TEMPLATE" --reasoning-budget-message " [cut]" > models-test/onyx-reasoning.log 2>&1 &
PIDS+=($!)

for i in $(seq 1 100); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && curl -sf "$EMB_BASE/health" > /dev/null 2>&1 \
        && curl -sf "$NP_BASE/health" > /dev/null 2>&1 && curl -sf "$MM_BASE/health" > /dev/null 2>&1 \
        && curl -sf "$RB_BASE/health" > /dev/null 2>&1 && curl -sf "$RK_BASE/health" > /dev/null 2>&1 \
        && curl -sf "$DEC_BASE/health" > /dev/null 2>&1 && curl -sf "$OPS_BASE/health" > /dev/null 2>&1 \
        && curl -sf "$TT_BASE/health" > /dev/null 2>&1 && curl -sf "$MEM_BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done
if ! curl -sf "$BASE/health" > /dev/null || ! curl -sf "$EMB_BASE/health" > /dev/null ||
   ! curl -sf "$NP_BASE/health" > /dev/null || ! curl -sf "$MM_BASE/health" > /dev/null ||
   ! curl -sf "$RB_BASE/health" > /dev/null || ! curl -sf "$RK_BASE/health" > /dev/null ||
   ! curl -sf "$DEC_BASE/health" > /dev/null || ! curl -sf "$OPS_BASE/health" > /dev/null ||
   ! curl -sf "$TT_BASE/health" > /dev/null || ! curl -sf "$MEM_BASE/health" > /dev/null; then
    echo "error: one or more test servers did not become ready" >&2
    for log in models-test/onyx-*.log; do [ -f "$log" ] && { echo "== $log"; tail -50 "$log"; }; done
    exit 1
fi

echo "== endpoints"
check "GET /health" curl -sf "$BASE/health"
# One slot, so per-slot context and total context are the same 512 here; the
# 2-slot instance below is what pins them apart.
check "GET /props reports n_ctx" \
    json_has 'd["n_ctx"] == 512 and d["default_generation_settings"]["n_ctx"] == 512 and d["n_ctx_total"] == 512 and "chat_template" in d' "$BASE/props"
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
check "invalid token id returns 400" http_status 400 -X POST "$BASE/v1/completions" \
    -d '{"prompt":[999999999],"max_tokens":1}'
check "browser origin is rejected" http_status 403 -H 'Origin: https://example.com' "$BASE/health"
check "multiple choices are rejected" http_status 400 -X POST "$BASE/v1/completions" \
    -d '{"prompt":"hello","n":2}'
check "POST /v1/chat/completions" \
    json_has 'd["choices"][0]["message"]["role"] == "assistant" and d["choices"][0]["finish_reason"] == "length"' \
    -X POST "$BASE/v1/chat/completions" -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":8}'
check "chat timings present" \
    json_has '"prompt_ms" in d["timings"] and "predicted_per_second" in d["timings"]' \
    -X POST "$BASE/v1/chat/completions" -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":4}'
check "json_schema constrained output is valid JSON" \
    bash -c "curl -sf -X POST '$BASE/v1/chat/completions' -d '{\"messages\":[{\"role\":\"user\",\"content\":\"person\"}],\"max_tokens\":40,\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"schema\":{\"type\":\"object\",\"properties\":{\"age\":{\"type\":\"integer\"}},\"required\":[\"age\"],\"additionalProperties\":false}}}}' | python3 -c 'import json,sys; d=json.load(sys.stdin); json.loads(d[\"choices\"][0][\"message\"][\"content\"])'"
# Structured output cut by max_tokens is an error, never half an object
# (Song Deck's M1). The tiny model keeps writing keys, so a small budget
# always cuts it.
JSON_RF='"response_format":{"type":"json_schema","json_schema":{"schema":{"type":"object","properties":{"a":{"type":"string"}},"required":["a"],"additionalProperties":false}}}'
check "json_schema output cut by max_tokens answers 400" bash -c "
    curl -s -o models-test/trunc.json -w '%{http_code}' -X POST '$BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"max_tokens\":2,$JSON_RF}' | grep -q '^400$' &&
    python3 -c 'import json; e = json.load(open(\"models-test/trunc.json\"))[\"error\"]; assert e[\"type\"] == \"invalid_request_error\" and \"max_tokens\" in e[\"message\"], e'"
check "json_schema output cut by max_tokens ends the stream on an error frame" bash -c "
    curl -sN -X POST '$BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"max_tokens\":2,\"stream\":true,$JSON_RF}' > models-test/trunc.sse
    tail -n 2 models-test/trunc.sse | grep -q '\"code\":400' && ! grep -q 'DONE' models-test/trunc.sse"
check "response_format json_object cut by max_tokens answers 400" http_status 400 -X POST "$BASE/v1/chat/completions" \
    -d '{"messages":[{"role":"user","content":"x"}],"max_tokens":2,"response_format":{"type":"json_object"}}'
check "completions json_schema cut by max_tokens answers 400" http_status 400 -X POST "$BASE/v1/completions" \
    -d '{"prompt":"x","max_tokens":1,"json_schema":{"type":"object","properties":{"a":{"type":"string"}},"required":["a"]}}'
# Song Deck's own schemas (tests/schemas/song-deck/: deep nesting, enums,
# integer enums, nullable unions, arrays of objects, additionalProperties
# false, strict) must convert to a grammar, and the grammar must hold the
# output to the schema's shape: an object that opens with its first key.
song_deck_schema() { # schema-file first-key
    python3 - "$BASE" "$1" "$2" <<'SDEOF'
import json, sys, urllib.error, urllib.request
base, path, first_key = sys.argv[1:4]
schema = json.load(open(path))
body = {"messages": [{"role": "user", "content": "Write the plan."}], "max_tokens": 24, "stream": True,
        "response_format": {"type": "json_schema", "json_schema": {"name": "song_deck", "strict": True, "schema": schema}}}
req = urllib.request.Request(base + "/v1/chat/completions", data=json.dumps(body).encode(), method="POST")
text, error, done = "", None, False
for raw in urllib.request.urlopen(req):
    line = raw.decode().strip()
    if line == "data: [DONE]":
        done = True
    elif line.startswith("data: "):
        frame = json.loads(line[6:])
        if "error" in frame:
            error = frame["error"]
        for choice in frame.get("choices", []):
            text += choice["delta"].get("content") or ""
head = text.lstrip()
assert head.startswith("{") and head[1:].lstrip().startswith('"%s"' % first_key), text
if done:
    json.loads(text)          # it finished: the whole answer must parse
else:
    assert error and error["code"] == 400 and "max_tokens" in error["message"], (error, text)
SDEOF
}
check "Song Deck composition-plan schema (openai-strict) constrains chat output" \
    song_deck_schema tests/schemas/song-deck/composition-plan.openai-strict.json key
check "Song Deck blueprint schema (json-schema, constraints kept) constrains chat output" \
    song_deck_schema tests/schemas/song-deck/blueprint.json-schema.json title
python3 -c 'print("{\"content\":" + "[" * 65 + "]" * 65 + "}")' > models-test/deep.json
check "a request body nested deeper than 64 levels answers 400" http_status 400 -X POST "$BASE/tokenize" \
    --data-binary @models-test/deep.json
check "an HTTP/1.1 request without Host answers 400" python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $PORT))
s.sendall(b'GET /health HTTP/1.1\\r\\nConnection: close\\r\\n\\r\\n')
assert s.recv(64).startswith(b'HTTP/1.1 400'), 'expected 400'"
check "KV prefix reuse reports cache_n" bash -c "
    curl -sf -X POST '$BASE/v1/completions' -d '{\"prompt\":\"The quick brown fox jumps over\",\"max_tokens\":4}' > /dev/null
    curl -sf -X POST '$BASE/v1/completions' -d '{\"prompt\":\"The quick brown fox jumps over\",\"max_tokens\":4}' \
      | python3 -c 'import json,sys; assert json.load(sys.stdin)[\"timings\"][\"cache_n\"] > 0'"

echo "== streaming"
STREAM=$(curl -sfN -X POST "$BASE/v1/chat/completions" -d '{"messages":[{"role":"user","content":"Hi"}],"max_tokens":5,"stream":true}')
check "SSE data frames"     grep -q '^data: {' <<< "$STREAM"
check "SSE [DONE] frame"    grep -q '^data: \[DONE\]' <<< "$STREAM"
check "SSE final usage"     grep -q '"usage"' <<< "$STREAM"
check "SSE finish_reason"   grep -q '"finish_reason":"length"' <<< "$STREAM"

echo "== embeddings instance"
check "POST /v1/embeddings" \
    json_has 'len(d["data"]) == 2 and len(d["data"][0]["embedding"]) == 64 and d["usage"]["prompt_tokens"] > 0' \
    -X POST "$EMB_BASE/v1/embeddings" -d '{"input":["hello world","goodbye"]}'
check "mixed embedding token array returns 400" http_status 400 -X POST "$EMB_BASE/v1/embeddings" \
    -d '{"input":[5,"a"]}'
# the official OpenAI clients send encoding_format "base64" by default
check "encoding_format base64 decodes to the float vector" bash -c "
    f=\$(curl -sf -X POST '$EMB_BASE/v1/embeddings' -d '{\"input\":\"hello world\"}')
    b=\$(curl -sf -X POST '$EMB_BASE/v1/embeddings' -d '{\"input\":\"hello world\",\"encoding_format\":\"base64\"}')
    python3 -c '
import base64, json, struct, sys
f = json.loads(sys.argv[1])[\"data\"][0][\"embedding\"]
raw = base64.b64decode(json.loads(sys.argv[2])[\"data\"][0][\"embedding\"])
b = struct.unpack(\"<%df\" % (len(raw) // 4), raw)
assert len(b) == len(f) == 64 and max(abs(x - y) for x, y in zip(f, b)) < 1e-6, (f[:3], b[:3])
' \"\$f\" \"\$b\""
check "an unknown encoding_format returns 400" http_status 400 -X POST "$EMB_BASE/v1/embeddings" \
    -d '{"input":"x","encoding_format":"binary"}'
check "generation rejected in embedding mode (501)" bash -c "
    code=\$(curl -s -o /dev/null -w '%{http_code}' -X POST '$EMB_BASE/v1/chat/completions' -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}]}')
    [ \"\$code\" = 501 ]"
check "embeddings rejected in generation mode (501)" bash -c "
    code=\$(curl -s -o /dev/null -w '%{http_code}' -X POST '$BASE/v1/embeddings' -d '{\"input\":\"x\"}')
    [ \"\$code\" = 501 ]"

echo "== rank-pooling instance"
# The tiny llama model has no classifier head and no labels, so n_cls_out is
# llama.cpp's default 1 and the "score" is the first pooled hidden value. One
# raw (unnormalized) value per input; an L2-normalized single value would be
# exactly +/-1.
check "POST /v1/embeddings with --pooling rank returns one raw score per input" \
    json_has 'len(d["data"]) == 2 and all(len(e["embedding"]) == 1 and abs(abs(e["embedding"][0]) - 1.0) > 1e-6 for e in d["data"])' \
    -X POST "$RK_BASE/v1/embeddings" -d '{"input":["hello world","goodbye"]}'

echo "== parallel slots (-np 2) instance"
# /props n_ctx is PER-SLOT, matching llama-server (its get_res_props reports
# default_generation_settings.n_ctx as meta.slot_n_ctx). This instance runs
# -c 512 -np 2, so llama.cpp divides the context and each slot gets 256 --
# and 256 is the window a single request is measured against. JX Runtime
# passes --parallel on every launch and records this number as the model's
# context length, so reporting the undivided 512 here made it admit prompts
# the engine then rejected. The undivided value stays available as
# n_ctx_total. Both read paths (top-level and nested) must agree.
check "GET /props reports per-slot n_ctx on the 2-slot instance" \
    json_has 'd["n_ctx"] == 256 and d["default_generation_settings"]["n_ctx"] == 256 and d["n_ctx_total"] == 512' \
    "$NP_BASE/props"
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

echo "== multimodal (--mmproj)"
check "GET /props reports no modalities without --mmproj" \
    json_has 'd["modalities"]["vision"] is False and d["modalities"]["audio"] is False' "$BASE/props"
# a bad projector must fail loudly at startup rather than silently serving
# text-only, so the runtime never thinks it has vision when it does not
GARBAGE_MMPROJ="$(mktemp)"
printf 'not a gguf file at all\n' > "$GARBAGE_MMPROJ"
check "--mmproj with a garbage file fails at startup" bash -c "
    out=\$('$BIN' -m '$MODEL' --port $((PORT + 9)) --mmproj '$GARBAGE_MMPROJ' 2>&1); rc=\$?
    [ \"\$rc\" != 0 ] && grep -qi mmproj <<< \"\$out\""
check "--mmproj with a nonexistent file fails at startup" bash -c "
    out=\$('$BIN' -m '$MODEL' --port $((PORT + 9)) --mmproj /nonexistent/projector.gguf 2>&1); rc=\$?
    [ \"\$rc\" != 0 ] && grep -qi mmproj <<< \"\$out\""
rm -f "$GARBAGE_MMPROJ"
# 1x1 transparent PNG
TINY_PNG_URI="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
check "image_url part without --mmproj is rejected (400)" bash -c "
    body=\$(curl -s -w '\n%{http_code}' -X POST '$BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"what is this\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"$TINY_PNG_URI\"}}]}],\"max_tokens\":4}')
    [ \"\$(tail -1 <<< \"\$body\")\" = 400 ] && grep -qi mmproj <<< \"\$body\""
check "remote image URLs are rejected (400)" bash -c "
    body=\$(curl -s -w '\n%{http_code}' -X POST '$BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat.png\"}}]}],\"max_tokens\":4}')
    [ \"\$(tail -1 <<< \"\$body\")\" = 400 ]"

echo "== multimodal instance (--mmproj)"
check "GET /props reports vision on the --mmproj instance" \
    json_has 'd["modalities"]["vision"] is True and d["modalities"]["audio"] is False' "$MM_BASE/props"
check "audio parts rejected by a vision-only projector (400)" bash -c "
    body=\$(curl -s -w '\n%{http_code}' -X POST '$MM_BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"UklGRg==\",\"format\":\"wav\"}}]}],\"max_tokens\":4}')
    [ \"\$(tail -1 <<< \"\$body\")\" = 400 ] && grep -qi audio <<< \"\$body\""
# an 8x8 RGB PNG, built here so the test needs no binary fixtures
IMG_B64=$(python3 - <<'PYEOF'
import base64, struct, zlib
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
w = h = 8
rows = b''
for y in range(h):
    rows += b'\x00' + b''.join(bytes([(x * 30) % 256, (y * 30) % 256, 128]) for x in range(w))
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(rows))
       + chunk(b'IEND', b''))
print(base64.b64encode(png).decode())
PYEOF
)
IMG_REQ="{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"describe this\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,$IMG_B64\"}}]}],\"max_tokens\":6,\"temperature\":0}"
# the projector contributes 4 image positions, so prompt_tokens must exceed the
# token count of the text alone
check "chat completion with a data: URI image" \
    json_has 'd["choices"][0]["message"]["role"] == "assistant" and d["usage"]["completion_tokens"] == 6 and d["usage"]["prompt_tokens"] > 4' \
    -X POST "$MM_BASE/v1/chat/completions" -d "$IMG_REQ"
check "a second image request reuses the slot cleanly" \
    json_has 'd["usage"]["completion_tokens"] == 6 and d["timings"]["cache_n"] == 0' \
    -X POST "$MM_BASE/v1/chat/completions" -d "$IMG_REQ"
check "raw base64 (no data: URI) is accepted" \
    json_has 'd["usage"]["completion_tokens"] == 4' \
    -X POST "$MM_BASE/v1/chat/completions" \
    -d "{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"$IMG_B64\"}}]}],\"max_tokens\":4}"
check "text-only requests still work on the --mmproj instance" \
    json_has 'd["usage"]["completion_tokens"] == 5' \
    -X POST "$MM_BASE/v1/completions" -d '{"prompt":"Once upon a time","max_tokens":5}'
check "streaming works with an image" bash -c "
    out=\$(curl -sfN -X POST '$MM_BASE/v1/chat/completions' -d '$(python3 -c "
import json,sys
req = json.loads(sys.argv[1]); req['stream'] = True; print(json.dumps(req))
" "$IMG_REQ")')
    grep -q '^data: \[DONE\]' <<< \"\$out\" && grep -q '\"finish_reason\":\"length\"' <<< \"\$out\""

echo "== logprobs"
check "chat logprobs: content[] matches completion_tokens, well-formed entries" bash -c "
    curl -sf -X POST '$BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],\"max_tokens\":6,\"logprobs\":true,\"top_logprobs\":3}' \
    | python3 -c '
import json, sys
d = json.load(sys.stdin)
content = d[\"choices\"][0][\"logprobs\"][\"content\"]
# an EOG token counts in completion_tokens but has no text and no entry,
# so a natural stop is one entry short of the token count (OpenAI behavior)
n = d[\"usage\"][\"completion_tokens\"]
if d[\"choices\"][0][\"finish_reason\"] == \"length\":
    assert len(content) == n, content
else:
    assert len(content) in (n, n - 1), content
def check_bytes(e):
    # bytes is always the raw token bytes. token is a JSON string, so a
    # byte-fallback token (a lone UTF-8 continuation byte such as <0x88>,
    # which the random model samples now and then) cannot round-trip:
    # it is serialized as U+FFFD, exactly as OpenAI does. Require equality
    # only when the string decodes cleanly; otherwise require a raw byte
    # list that is not the encoding of the replacement character.
    assert isinstance(e[\"bytes\"], list) and all(isinstance(b, int) and 0 <= b <= 255 for b in e[\"bytes\"]), e
    if \"\\ufffd\" not in e[\"token\"]:
        assert e[\"bytes\"] == list(e[\"token\"].encode(\"utf-8\")), e
    else:
        assert e[\"bytes\"] and e[\"bytes\"] != list(\"\\ufffd\".encode(\"utf-8\")), e
for e in content:
    assert isinstance(e[\"token\"], str)
    assert e[\"logprob\"] <= 0
    check_bytes(e)
    assert len(e[\"top_logprobs\"]) == 3
    lps = [t[\"logprob\"] for t in e[\"top_logprobs\"]]
    assert lps == sorted(lps, reverse=True), lps
    for t in e[\"top_logprobs\"]:
        check_bytes(t)
'"
check "top_logprobs without logprobs is rejected (400)" bash -c "
    code=\$(curl -s -o /dev/null -w '%{http_code}' -X POST '$BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],\"max_tokens\":4,\"top_logprobs\":3}')
    [ \"\$code\" = 400 ]"
check "chat logprobs streaming: frames carry as many entries as completion_tokens" bash -c "
    curl -sfN -X POST '$BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],\"max_tokens\":6,\"stream\":true,\"logprobs\":true,\"top_logprobs\":2}' \
    | python3 -c '
import json, sys
n = 0
usage = None
for line in sys.stdin:
    line = line.strip()
    if not line.startswith(\"data: \") or line == \"data: [DONE]\":
        continue
    d = json.loads(line[6:])
    if d.get(\"usage\"):
        usage = d[\"usage\"]
    if not d.get(\"choices\"):
        continue
    lp = d[\"choices\"][0].get(\"logprobs\")
    if lp:
        n += len(lp[\"content\"])
assert usage is not None
# an EOG token counts in completion_tokens but carries no logprobs entry
assert n in (usage[\"completion_tokens\"], usage[\"completion_tokens\"] - 1), (n, usage)
'"
check "completions legacy logprobs shape" bash -c "
    curl -sf -X POST '$BASE/v1/completions' -d '{\"prompt\":\"Once upon a time\",\"max_tokens\":6,\"logprobs\":2}' \
    | python3 -c '
import json, sys
d = json.load(sys.stdin)
lp = d[\"choices\"][0][\"logprobs\"]
n = len(lp[\"tokens\"])
# an EOG token counts in completion_tokens but has no text and no entry
assert n in (d[\"usage\"][\"completion_tokens\"], d[\"usage\"][\"completion_tokens\"] - 1), lp
assert len(lp[\"token_logprobs\"]) == n
assert len(lp[\"top_logprobs\"]) == n
assert len(lp[\"text_offset\"]) == n
assert n == 0 or lp[\"text_offset\"][0] == 0
assert \"\".join(lp[\"tokens\"]) == d[\"choices\"][0][\"text\"]
for tlp in lp[\"top_logprobs\"]:
    assert len(tlp) == 2
'"

echo "== reasoning budget (--reasoning-budget)"
check "--help lists --reasoning-budget" bash -c "'$BIN' --help | grep -q -- --reasoning-budget"
check "generation prompt renders inside the thinking block" \
    json_has '"assistant:<think>" == d["prompt"][-len("assistant:<think>"):]' \
    -X POST "$RB_BASE/apply-template" -d '{"messages":[{"role":"user","content":"Hi"}]}'
check "reasoning_budget_tokens=4 forces the closing tag with the injected message" bash -c "
    curl -sf -X POST '$RB_BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],\"max_tokens\":40,\"temperature\":0,\"reasoning_budget_tokens\":4}' \
    | python3 -c '
import json, sys
d = json.load(sys.stdin)
text = json.dumps(d)
assert \" [cut]\" in text, text
assert \"</think>\" in text, text
'"
check "reasoning_budget_tokens=0 forces immediately with no injected message" bash -c "
    curl -sf -X POST '$RB_BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],\"max_tokens\":40,\"temperature\":0,\"reasoning_budget_tokens\":0}' \
    | python3 -c '
import json, sys
d = json.load(sys.stdin)
content = d[\"choices\"][0][\"message\"][\"content\"]
assert \"[cut]\" not in content, content
assert \"</think>\" in content, content
# forced immediately: the closing tag must appear at (or extremely near) the
# very start of the generated text, well before a budget=4 response would see it
assert content.index(\"</think>\") <= 4, content
'"
check "default budget (-1) on the same template does not force" bash -c "
    curl -sf -X POST '$RB_BASE/v1/chat/completions' \
      -d '{\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],\"max_tokens\":40,\"temperature\":0}' \
    | python3 -c '
import json, sys
d = json.load(sys.stdin)
text = json.dumps(d)
assert \"[cut]\" not in text, text
assert \"</think>\" not in text, text
'"
rm -f "$RB_TEMPLATE"

echo "== decision models (/v1/systemone)"
SYSTEMONE_BODY='{"state":"I was charged twice and nobody replied.","questions":{
  "route":{"type":"choice","instructions":"Which team?","criteria":{"billing":"payments","shipping":null,"technical":null}},
  "urgency":{"type":"score","instructions":"How urgent?","criteria":["can wait","this week","today","right now"]},
  "angry":{"type":"noul","instructions":"Is the customer angry?"}}}'
check "GET /props reports the decision type" json_has 'd["decision_type"] == "openjev"' "$DEC_BASE/props"
check "GET /props on a text model has no decision type" json_has 'd["decision_type"] is None' "$BASE/props"
check "POST /v1/systemone answers every question in order" bash -c "
    curl -sf -X POST '$DEC_BASE/v1/systemone' -d '$SYSTEMONE_BODY' | python3 -c '
import json, sys
d = json.load(sys.stdin)
a = d[\"answers\"]
assert d[\"model\"] == \"tiny-decision\", d
assert list(a) == [\"route\", \"urgency\", \"angry\"], a
assert d[\"usage\"][\"input_tokens\"] > 0 and d[\"usage\"][\"output_tokens\"] == 0, d
r = a[\"route\"]
assert r[\"type\"] == \"choice\" and list(r[\"probabilities\"]) == [\"billing\", \"shipping\", \"technical\"], r
assert abs(sum(r[\"probabilities\"].values()) - 1) < 1e-6, r
assert r[\"choice\"] == max(r[\"probabilities\"], key=r[\"probabilities\"].get), r
assert 0 <= r[\"confidence\"] <= 1, r
u = a[\"urgency\"]
assert u[\"legend\"] == {\"0\": \"can wait\", \"1\": \"this week\", \"2\": \"today\", \"3\": \"right now\"}, u
assert abs(u[\"score\"] - sum(i * p for i, p in enumerate(u[\"probabilities\"].values()))) < 1e-6, u
assert 0 <= a[\"angry\"][\"noul\"] <= 1, a
'"
# the questions of one request share their prompt prefix, evaluated once; the
# answer must not depend on that (flash attention may move it by ~1e-4)
check "a question answers the same alone and with others" bash -c "
    together=\$(curl -sf -X POST '$DEC_BASE/v1/systemone' -d '$SYSTEMONE_BODY')
    alone=\$(curl -sf -X POST '$DEC_BASE/v1/systemone' \
      -d '{\"state\":\"I was charged twice and nobody replied.\",\"questions\":{\"angry\":{\"type\":\"noul\",\"instructions\":\"Is the customer angry?\"}}}')
    python3 -c '
import json, sys
t = json.loads(sys.argv[1])[\"answers\"][\"angry\"][\"noul\"]
a = json.loads(sys.argv[2])[\"answers\"][\"angry\"][\"noul\"]
assert abs(t - a) < 1e-3, (t, a)
' \"\$together\" \"\$alone\""
check "missing state returns 400" http_status 400 -X POST "$DEC_BASE/v1/systemone" \
    -d '{"questions":{"q":{"type":"noul","instructions":"x"}}}'
check "unknown question type returns 400" http_status 400 -X POST "$DEC_BASE/v1/systemone" \
    -d '{"state":"x","questions":{"q":{"type":"maybe","instructions":"x"}}}'
check "score with one level returns 400" http_status 400 -X POST "$DEC_BASE/v1/systemone" \
    -d '{"state":"x","questions":{"q":{"type":"score","instructions":"x","criteria":["one"]}}}'
check "too many options returns 400" bash -c "
    body=\$(python3 -c 'import json; print(json.dumps({\"state\":\"x\",\"questions\":{\"q\":{\"type\":\"choice\",\"instructions\":\"x\",\"criteria\":{str(i): None for i in range(53)}}}}))')
    [ \"\$(curl -s -o /dev/null -w '%{http_code}' -X POST '$DEC_BASE/v1/systemone' -d \"\$body\")\" = 400 ]"
check "images without a projector return 501" http_status 501 -X POST "$DEC_BASE/v1/systemone" \
    -d '{"state":"x","images":["data:image/png;base64,iVBORw0KGgo="],"questions":{"q":{"type":"noul","instructions":"x"}}}'
check "chat completions on a decision model return 501" http_status 501 -X POST "$DEC_BASE/v1/chat/completions" \
    -d '{"messages":[{"role":"user","content":"Hi"}]}'
check "/v1/systemone on a text model returns 501" http_status 501 -X POST "$BASE/v1/systemone" -d "$SYSTEMONE_BODY"

echo "== readiness and port selection"
# A safetensors directory makes the engine run the converter before it loads
# anything. Pointing ONYX_ENGINE_CONVERT_SCRIPT at a stub that waits for a
# flag file holds the engine in its loading state for as long as the test
# needs; the stub then fails, which fails the load.
LOAD_DIR="models-test/loading-model"
LOAD_FLAG="models-test/loading.release"
rm -rf "$LOAD_DIR" "$LOAD_FLAG" models-test/loading.exit
mkdir -p "$LOAD_DIR"
echo '{"architectures":["LlamaForCausalLM"]}' > "$LOAD_DIR/config.json"
printf 'x' > "$LOAD_DIR/model.safetensors"
cat > models-test/stub-converter.py <<PY
import os, sys, time
while not os.path.exists("$LOAD_FLAG"):
    time.sleep(0.05)
print("stub converter: refusing on purpose", file=sys.stderr)
sys.exit(3)
PY
( rc=0
  ONYX_ENGINE_CONVERT_SCRIPT="$(native_path models-test/stub-converter.py)" \
      "$BIN" -m "$LOAD_DIR" --port "$LOAD_PORT" > models-test/onyx-loading.log 2>&1 || rc=$?
  echo "exit=$rc" > models-test/loading.exit ) &
PIDS+=($!)
for i in $(seq 1 100); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' "$LOAD_BASE/health")" = 503 ] && break
    sleep 0.1
done
check "while loading, /health answers 503 'Loading model'" bash -c "
    [ \"\$(curl -s -o models-test/load-body.json -w '%{http_code}' '$LOAD_BASE/health')\" = 503 ] &&
    python3 -c 'import json; d = json.load(open(\"models-test/load-body.json\")); assert d[\"error\"][\"type\"] == \"unavailable_error\" and d[\"error\"][\"message\"] == \"Loading model\", d'"
check "while loading, model routes answer 503" http_status 503 -X POST "$LOAD_BASE/v1/chat/completions" \
    -d '{"messages":[{"role":"user","content":"Hi"}]}'
check "while loading, /props answers 503" http_status 503 "$LOAD_BASE/props"
check "while loading, stderr already reports the listening port" \
    grep -q "listening on http://127.0.0.1:$LOAD_PORT (loading)" models-test/onyx-loading.log
touch "$LOAD_FLAG"
for i in $(seq 1 100); do [ -f models-test/loading.exit ] && break; sleep 0.1; done
check "a failed load exits 1 with an error line" bash -c "
    grep -q '^exit=1$' models-test/loading.exit && grep -q '^error:' models-test/onyx-loading.log"
check "after a failed load the port is closed" bash -c "! curl -s '$LOAD_BASE/health' > /dev/null"
check "once loaded, /health answers 200 ok" json_has 'd["status"] == "ok"' "$BASE/health"
check "stderr reports ready with the bound port" grep -q "ready on http://127.0.0.1:$PORT" models-test/onyx-generation.log
check "/props reports the bound port" json_has "d['port'] == $PORT" "$BASE/props"

"$BIN" -m "$MODEL" --port 0 -c 256 --alias tiny-port0 > models-test/onyx-port0.log 2>&1 &
PORT0_PID=$!
PIDS+=($PORT0_PID)
PORT0=""
for i in $(seq 1 100); do
    PORT0=$(sed -n 's|.*listening on http://127.0.0.1:\([0-9]*\) (loading).*|\1|p' models-test/onyx-port0.log)
    [ -n "$PORT0" ] && curl -sf "http://127.0.0.1:$PORT0/health" > /dev/null 2>&1 && break
    sleep 0.1
done
check "--port 0 binds a free port and reports it on stderr" test -n "$PORT0"
check "--port 0: /props reports the same port" json_has "d['port'] == int('${PORT0:-0}')" "http://127.0.0.1:${PORT0:-1}/props"
kill "$PORT0_PID" 2> /dev/null || true

echo "== operations (X-Request-Id, auth, Host allowlist, /metrics, --max-queue, --tick-tokens)"
AUTH=(-H "Authorization: Bearer $OPS_KEY")
check "/health needs no key" curl -sf "$OPS_BASE/health"
check "a missing key answers 401" http_status 401 "$OPS_BASE/v1/models"
check "a wrong key of the same length answers 401" http_status 401 -H "Authorization: Bearer sk-smokf" "$OPS_BASE/v1/models"
check "a key with a longer suffix answers 401" http_status 401 -H "Authorization: Bearer ${OPS_KEY}x" "$OPS_BASE/v1/models"
check "the right key answers 200" http_status 200 "${AUTH[@]}" "$OPS_BASE/v1/models"
check "X-Request-Id is echoed" bash -c "
    curl -s -D - -o /dev/null -H 'Authorization: Bearer $OPS_KEY' -H 'X-Request-Id: req-123.abc' '$OPS_BASE/v1/models' |
    tr -d '\r' | grep -qi '^x-request-id: req-123.abc$'"
check "X-Request-Id is echoed on errors too" bash -c "
    curl -s -D - -o /dev/null -H 'X-Request-Id: req-401' '$OPS_BASE/v1/models' | tr -d '\r' | grep -qi '^x-request-id: req-401$'"
check "an X-Request-Id with spaces is not echoed" bash -c "
    ! curl -s -D - -o /dev/null -H 'Authorization: Bearer $OPS_KEY' -H 'X-Request-Id: a b' '$OPS_BASE/v1/models' | grep -qi '^x-request-id'"
check "a foreign Host header answers 403" http_status 403 -H "Host: evil.example:$OPS_PORT" "$OPS_BASE/health"
check "Host: localhost is accepted" http_status 200 -H "Host: localhost:$OPS_PORT" "$OPS_BASE/health"
check "/metrics is off without --metrics" http_status 404 "$BASE/metrics"
check "/metrics needs the key" http_status 401 "$OPS_BASE/metrics"
curl -sf "${AUTH[@]}" -X POST "$OPS_BASE/v1/completions" -d '{"prompt":"Once upon a time","max_tokens":8}' > /dev/null || true
check "/metrics counts tokens under llamacpp:* names" bash -c "
    curl -sf -H 'Authorization: Bearer $OPS_KEY' '$OPS_BASE/metrics' > models-test/metrics.txt &&
    grep -q '^# TYPE llamacpp:tokens_predicted_total counter' models-test/metrics.txt &&
    awk '/^llamacpp:tokens_predicted_total /{exit !(\$2 >= 8)}' models-test/metrics.txt &&
    awk '/^llamacpp:prompt_tokens_total /{exit !(\$2 > 0)}' models-test/metrics.txt &&
    grep -q '^llamacpp:requests_deferred 0' models-test/metrics.txt"
# One slot and --max-queue 1: of twenty simultaneous requests, one runs, one
# waits, and the rest are refused until those finish.
check "--max-queue answers 503 with Retry-After when the queue is full" bash -c "
    rm -rf models-test/mq && mkdir -p models-test/mq
    for i in \$(seq 1 20); do
        curl -s -D models-test/mq/h\$i -o models-test/mq/b\$i -H 'Authorization: Bearer $OPS_KEY' \
          -X POST '$OPS_BASE/v1/completions' -d '{\"prompt\":\"Once upon a time\",\"max_tokens\":400}' &
    done
    wait
    ok=\$(grep -l '^HTTP/1.1 200' models-test/mq/h* | wc -l)
    busy=\$(grep -l '^HTTP/1.1 503' models-test/mq/h* | wc -l)
    retry=\$(grep -il '^retry-after: 1' models-test/mq/h* | wc -l)
    echo \"200: \$ok, 503: \$busy, Retry-After: \$retry\"
    [ \$ok -ge 1 ] && [ \$busy -ge 1 ] && [ \$retry -eq \$busy ] &&
    grep -q unavailable_error \$(grep -l '^HTTP/1.1 503' models-test/mq/h* | head -1 | sed 's|/h|/b|')"
check "/metrics counts refused requests" bash -c "
    curl -sf -H 'Authorization: Bearer $OPS_KEY' '$OPS_BASE/metrics' | awk '/^onyx:requests_rejected_total /{exit !(\$2 >= 1)}'"
echo "== memory flags and --queue-timeout"
check "f32/bf16 KV cache and tensor overrides: generation works" \
    json_has 'd["usage"]["completion_tokens"] == 4' -X POST "$MEM_BASE/v1/completions" -d '{"prompt":"Once upon a time","max_tokens":4}'
# One slot and --queue-timeout 1: six requests of 400 tokens take well over
# a second one after another, so the last ones wait past it and get the 503.
check "--queue-timeout answers 503 with Retry-After after waiting for a slot" bash -c "
    rm -rf models-test/qt && mkdir -p models-test/qt
    for i in 1 2 3 4 5 6; do
        curl -s -D models-test/qt/h\$i -o models-test/qt/b\$i \
          -X POST '$MEM_BASE/v1/completions' -d '{\"prompt\":\"Once upon a time\",\"max_tokens\":400}' &
    done
    wait
    ok=\$(grep -l '^HTTP/1.1 200' models-test/qt/h* | wc -l)
    busy=\$(grep -l '^HTTP/1.1 503' models-test/qt/h* | wc -l)
    retry=\$(grep -il '^retry-after: 1' models-test/qt/h* | wc -l)
    echo \"200: \$ok, 503: \$busy, Retry-After: \$retry\"
    [ \$ok -ge 1 ] && [ \$busy -ge 1 ] && [ \$retry -eq \$busy ] &&
    grep -q 'queue-timeout' \$(grep -l '^HTTP/1.1 503' models-test/qt/h* | head -1 | sed 's|/h|/b|')"
check "/metrics counts timed-out requests" bash -c "
    curl -sf '$MEM_BASE/metrics' | awk '/^onyx:requests_timed_out_total /{exit !(\$2 >= 1)}'"

LONG_PROMPT=$(python3 -c 'print(" ".join(["word"] * 120))')
check "--tick-tokens 16: two concurrent long prompts still complete" bash -c "
    for i in 1 2; do
        curl -sf -X POST '$TT_BASE/v1/completions' -d '{\"prompt\":\"$LONG_PROMPT\",\"max_tokens\":8}' > models-test/tt\$i.json &
    done
    wait
    for i in 1 2; do
        python3 -c 'import json,sys; d = json.load(open(sys.argv[1])); assert d[\"usage\"][\"completion_tokens\"] == 8 and d[\"usage\"][\"prompt_tokens\"] > 100, d' models-test/tt\$i.json || exit 1
    done"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
