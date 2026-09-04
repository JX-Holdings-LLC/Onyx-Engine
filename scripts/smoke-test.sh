#!/usr/bin/env bash
# End-to-end smoke test for onyx-engine using the offline tiny test model.
#
# Usage: scripts/smoke-test.sh [path-to-onyx-engine-binary]
#
# Requires: python3 + numpy (to generate the test model), curl.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN="${1:-build/onyx-engine}"
MODEL="models-test/tiny-llama-random.gguf"
MMPROJ="models-test/tiny-mmproj-random.gguf"
PORT="${ONYX_SMOKE_PORT:-18190}"
EMB_PORT=$((PORT + 1))
NP_PORT=$((PORT + 2))
MM_PORT=$((PORT + 3))
RB_PORT=$((PORT + 4))
BASE="http://127.0.0.1:$PORT"
EMB_BASE="http://127.0.0.1:$EMB_PORT"
NP_BASE="http://127.0.0.1:$NP_PORT"
MM_BASE="http://127.0.0.1:$MM_PORT"
RB_BASE="http://127.0.0.1:$RB_PORT"

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

json_has() { # url-args... jq-ish python expression reading parsed json as d
    local expr="$1"; shift
    curl -sf "$@" | python3 -c "import json,sys; d=json.load(sys.stdin); assert $expr, d"
}

[ -x "$BIN" ] || { echo "error: binary '$BIN' not found (build first)"; exit 1; }
[ -f "$MODEL" ]  || python3 scripts/make-tiny-model.py "$MODEL"
[ -f "$MMPROJ" ] || python3 scripts/make-tiny-mmproj.py "$MMPROJ"

echo "== CLI"
check "--version" bash -c "'$BIN' --version | grep -q onyx-engine"
check "--help lists --cache-reuse" bash -c "'$BIN' --help | grep -q -- --cache-reuse"
check "--help lists --parallel" bash -c "'$BIN' --help | grep -q -- --parallel"
check "--help lists --context-shift" bash -c "'$BIN' --help | grep -q -- --context-shift"
check "--help lists --mmproj" bash -c "'$BIN' --help | grep -q -- --mmproj"
check "--help lists --no-webui" bash -c "'$BIN' --help | grep -q -- --no-webui"

echo "== starting generation instance on :$PORT"
"$BIN" -m "$MODEL" --port "$PORT" -c 512 --alias tiny-test > /dev/null 2>&1 &
PIDS+=($!)
echo "== starting embedding instance on :$EMB_PORT"
"$BIN" -m "$MODEL" --port "$EMB_PORT" --embedding --pooling mean > /dev/null 2>&1 &
PIDS+=($!)
echo "== starting 2-slot context-shift instance on :$NP_PORT"
"$BIN" -m "$MODEL" --port "$NP_PORT" -c 512 -np 2 --context-shift --alias tiny-np2 > /dev/null 2>&1 &
PIDS+=($!)
echo "== starting multimodal instance on :$MM_PORT"
"$BIN" -m "$MODEL" --port "$MM_PORT" -c 512 --mmproj "$MMPROJ" --alias tiny-mm > /dev/null 2>&1 &
PIDS+=($!)

# a deepseek-style template whose generation prompt itself ends inside the
# thinking block ("...assistant:<think>"), used by the --reasoning-budget
# tests below; written with printf (not a heredoc) so it carries no trailing
# newline, which would otherwise land inside the rendered prompt.
RB_TEMPLATE="$(mktemp)"
printf '{%% for m in messages %%}{{ m["role"] }}: {{ m["content"] }}\n{%% endfor %%}{%% if add_generation_prompt %%}assistant:<think>{%% endif %%}' > "$RB_TEMPLATE"
echo "== starting reasoning-budget instance on :$RB_PORT"
"$BIN" -m "$MODEL" --port "$RB_PORT" -c 512 --alias tiny-rb \
    --chat-template-file "$RB_TEMPLATE" --reasoning-budget-message " [cut]" > /dev/null 2>&1 &
PIDS+=($!)

for i in $(seq 1 100); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && curl -sf "$EMB_BASE/health" > /dev/null 2>&1 \
        && curl -sf "$NP_BASE/health" > /dev/null 2>&1 && curl -sf "$MM_BASE/health" > /dev/null 2>&1 \
        && curl -sf "$RB_BASE/health" > /dev/null 2>&1 && break
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
for e in content:
    assert isinstance(e[\"token\"], str)
    assert e[\"logprob\"] <= 0
    assert e[\"bytes\"] == list(e[\"token\"].encode(\"utf-8\"))
    assert len(e[\"top_logprobs\"]) == 3
    lps = [t[\"logprob\"] for t in e[\"top_logprobs\"]]
    assert lps == sorted(lps, reverse=True), lps
    for t in e[\"top_logprobs\"]:
        assert t[\"bytes\"] == list(t[\"token\"].encode(\"utf-8\"))
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

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
