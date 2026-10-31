[Documentation](../README.md) › [Guides](README.md) › Supervising Onyx from your own app

# Supervising Onyx from your own app

The job: start `onyx-engine` as a child process of your program, know when it is ready, send it
requests, and stop it cleanly. This page is enough to write that supervisor in any language. JX
Runtime does the same job, but you do not need to know anything about JX Runtime to follow it.
Everything used here is on the [stability page](../reference/stability.md).

## The lifecycle

| Phase | What the engine does | What you see |
|---|---|---|
| Spawn | Parses arguments. Bad ones exit `1` at once with an `error:` line. | Process exits `1`, or keeps running |
| Listening | Binds the port, then starts loading (and converting, for safetensors) | stderr `onyx-engine listening on http://HOST:PORT (loading)`; `/health` → `503` |
| Ready | The model is loaded and requests are served | stderr `onyx-engine ready on http://HOST:PORT (...)`; `/health` → `200 {"status":"ok"}` |
| Failed load | Prints `error: ...`, closes the port, exits `1` | Process exits `1`; connections are refused |
| Running | Serves until signalled | Normal responses; `503` + `Retry-After` when `--max-queue` is full or `--queue-timeout` passes |
| Stop | `SIGTERM` or `SIGINT` ends the process at once | Exit by signal; in-flight requests are cut off |

So there are three readiness answers, and each means one thing:

- **`503` with `"type":"unavailable_error"`**: the process is up and still loading. Keep waiting.
- **`200`**: ready.
- **Connection refused** before you have seen `503` or `200`: the port is not open yet, or the
  process has already exited. Check whether the process is still alive. Once you have seen a
  `503`, a refused connection means the process has exited or is exiting (a failed load closes the
  port, then exits `1`).

## Spawn arguments

Pass these on every launch, explicitly, rather than relying on defaults:

```text
onyx-engine -m MODEL --alias ID --host 127.0.0.1 --port PORT -c CTX -np SLOTS
```

- **`--alias`** sets the model id every response reports. Without it the id comes from the file
  name.
- **`--host 127.0.0.1`** (the default) keeps the engine on loopback. Any other host needs
  `--api-key`, or `--allow-unauthenticated` if you really mean it. See [security](security.md).
- **`--port 0`** lets the operating system choose a free port. Read it back from the
  `listening on` line on stderr, or from `GET /props` (`"port"`) once ready. This avoids the race
  of choosing a free port yourself and having something else take it before the engine binds.
- **`-c` and `-np`** size the context and the number of concurrent requests. `-c` is divided
  across the slots: `-c 16384 -np 4` gives each request 4096 tokens. `/props` reports the
  per-slot figure as `n_ctx`. See [slots and context](../architecture/concurrency.md#slots-and-context).
- **`--max-queue N`** bounds how many requests wait for a slot. Beyond it the engine answers
  `503` with `Retry-After: 1` instead of queueing without limit, which gives you a clear overload
  signal.
- **`--queue-timeout S`** bounds how long a request waits for a slot. A request still queued
  after `S` seconds gets the same `503` with `Retry-After: 1`, so a client never waits behind a
  stuck backlog longer than you chose.
- **`--metrics`** turns on `GET /metrics` (Prometheus text, llama.cpp's `llamacpp:*` names).

Probe before you pass anything newer than this page: `onyx-engine --help` lists every flag the
build accepts and nothing else, and `onyx-engine --version` names the build. Cache both per
binary.

## Concurrency and its per-slot effect

| Setting | Effect |
|---|---|
| `-np N` | `N` requests run at once, one continuous-batching loop. Request `N + 1` waits, first in first out. |
| `-c C` | Total context. Each slot gets `C / N` tokens, so raise `-c` when you raise `-np`. |
| `--max-queue Q` | At most `Q` requests wait. Request `N + Q + 1` gets `503`. `0` (default) means no limit. |
| `--queue-timeout S` | A request that waits `S` seconds without a slot gets `503`. `0` (default) means no limit. |
| `--tick-tokens T` | Caps the tokens per batching tick. Generating slots always get their one token first; long prompts are fed in the rest. A smaller `T` keeps streaming smooth while others prefill, at some cost to prompt throughput. |
| `--cache-reuse N` | Each slot reuses the KV of a shared prompt prefix of at least `N` tokens. `timings.cache_n` reports how many tokens were reused. |

## Health, properties and correlation

- `GET /health` needs no API key, so a supervisor can poll it without one.
- `GET /props` (after ready) gives `model_alias`, the per-slot `n_ctx`, `n_ctx_total`,
  `modalities`, `decision_type`, `build_info` and `port`. Read it once after the engine is
  ready.
- Send `X-Request-Id` with each request and the engine echoes it on the response (up to 128
  printable ASCII characters), so your logs and the engine's responses line up.

## What to read from stderr

stderr is for humans and logs. Two lines are stable and safe to parse:

```text
onyx-engine listening on http://127.0.0.1:41733 (loading)
onyx-engine ready on http://127.0.0.1:41733 (model: my-model, ctx: 4096)
```

Between them you see `onyx-engine <version>: loading model '...'` and
`onyx-engine: model loaded (...)`. A failure prints a line beginning `error:` and the process
exits `1`. Keep the last few dozen lines when a load fails; the cause is almost always in them.
Warnings from llama.cpp appear too; pass `-v` to see its full log.

## Stopping

Send `SIGTERM` (on Windows, terminate the process). The engine exits at once: it holds nothing
that needs saving, and in-flight requests end with a dropped connection. If your program needs
in-flight requests to finish, stop routing new requests to the engine, wait for the ones you sent
to complete, then signal it. Wait for the process to exit before reusing its port or starting
another engine on the same GPU.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | `--help` or `--version` |
| `1` | Invalid argument, refused `--host`, bind failure, conversion failure or load failure; see the `error:` line |
| by signal | You stopped it |

## Examples

Each example starts the engine on a free port, waits for ready, sends one chat request and stops
it. Set `ONYX_BIN` and `ONYX_MODEL` first.

### Shell

```bash
#!/usr/bin/env bash
set -euo pipefail
log=$(mktemp)
"$ONYX_BIN" -m "$ONYX_MODEL" --alias demo --port 0 -c 4096 -np 1 2> "$log" &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT

port=""
while [ -z "$port" ]; do
    kill -0 "$pid" 2>/dev/null || { cat "$log"; exit 1; }
    port=$(sed -n 's|.*listening on http://127.0.0.1:\([0-9]*\) (loading).*|\1|p' "$log")
    sleep 0.1
done

until [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/health")" = 200 ]; do
    kill -0 "$pid" 2>/dev/null || { cat "$log"; exit 1; }   # failed load
    sleep 0.2
done

curl -s "http://127.0.0.1:$port/v1/chat/completions" -H 'Content-Type: application/json' \
    -d '{"messages":[{"role":"user","content":"Hello!"}],"max_tokens":32}'
```

### Python

```python
import json, os, re, subprocess, threading, time, urllib.error, urllib.request

proc = subprocess.Popen(
    [os.environ["ONYX_BIN"], "-m", os.environ["ONYX_MODEL"], "--alias", "demo",
     "--port", "0", "-c", "4096", "-np", "1"],
    stderr=subprocess.PIPE, text=True)

# Read stderr on a thread: an unread pipe would eventually block the engine.
lines, port_found = [], threading.Event()
port = None
def read_stderr():
    global port
    for line in proc.stderr:
        lines.append(line)
        m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(loading\)", line)
        if m:
            port = int(m.group(1))
            port_found.set()
threading.Thread(target=read_stderr, daemon=True).start()

def fail():
    raise RuntimeError("onyx-engine exited:\n" + "".join(lines[-30:]))

while not port_found.wait(0.1):
    if proc.poll() is not None:
        fail()
base = f"http://127.0.0.1:{port}"

while True:
    if proc.poll() is not None:
        fail()
    try:
        with urllib.request.urlopen(base + "/health", timeout=2) as r:
            if r.status == 200:
                break
    except urllib.error.HTTPError as e:
        if e.code != 503:          # 503 means still loading
            raise
    except urllib.error.URLError:
        pass                       # not accepting yet
    time.sleep(0.2)

req = urllib.request.Request(
    base + "/v1/chat/completions",
    data=json.dumps({"messages": [{"role": "user", "content": "Hello!"}], "max_tokens": 32}).encode(),
    headers={"Content-Type": "application/json", "X-Request-Id": "demo-1"})
with urllib.request.urlopen(req) as r:
    print(json.load(r)["choices"][0]["message"]["content"])

proc.terminate()
proc.wait()
```

### Node.js

```js
const { spawn } = require('node:child_process');

async function startOnyx({ bin, model, alias, ctx = 4096, slots = 1 }) {
  const child = spawn(bin, ['-m', model, '--alias', alias, '--port', '0', '-c', String(ctx), '-np', String(slots)],
    { stdio: ['ignore', 'ignore', 'pipe'] });
  const tail = [];
  let exited = null;
  child.on('exit', (code, signal) => { exited = { code, signal }; });

  const port = await new Promise((resolve, reject) => {
    let buf = '';
    child.stderr.on('data', (chunk) => {
      buf += chunk;
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl);
        buf = buf.slice(nl + 1);
        tail.push(line);
        if (tail.length > 50) tail.shift();
        const m = /listening on http:\/\/127\.0\.0\.1:(\d+) \(loading\)/.exec(line);
        if (m) resolve(Number(m[1]));
      }
    });
    child.on('exit', () => reject(new Error('onyx-engine exited:\n' + tail.join('\n'))));
  });

  const base = `http://127.0.0.1:${port}`;
  for (;;) {
    if (exited) throw new Error('onyx-engine failed to load:\n' + tail.join('\n'));
    try {
      const res = await fetch(`${base}/health`, { signal: AbortSignal.timeout(2000) });
      if (res.ok) break;                       // 200: ready
      if (res.status !== 503) throw new Error(`unexpected /health status ${res.status}`);
    } catch (e) {
      if (!(e instanceof TypeError) && e.name !== 'TimeoutError') throw e; // refused or slow: retry
    }
    await new Promise((r) => setTimeout(r, 200));
  }
  return { child, base, stop: () => new Promise((r) => { child.once('exit', r); child.kill('SIGTERM'); }) };
}

(async () => {
  const onyx = await startOnyx({ bin: process.env.ONYX_BIN, model: process.env.ONYX_MODEL, alias: 'demo' });
  const res = await fetch(`${onyx.base}/v1/chat/completions`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-Request-Id': 'demo-1' },
    body: JSON.stringify({ messages: [{ role: 'user', content: 'Hello!' }], max_tokens: 32 }),
  });
  console.log((await res.json()).choices[0].message.content);
  await onyx.stop();
})();
```

## Handling overload and errors

| Status | Meaning | What to do |
|---|---|---|
| `400` | The request is wrong (an unsupported field, a prompt too long for a slot) | Fix the request; do not retry |
| `401` | Missing or wrong API key | Fix the key |
| `403` | The request carried `Origin`, or a `Host` that is not loopback on a loopback bind | Call it from your server process, by `127.0.0.1` or `localhost` |
| `413` | Body over `--max-request-bytes` (default 100 MiB) | Send smaller media, or raise the cap |
| `501` | The route is outside this process's mode (for example chat on an embedding model) | Start an engine in the right mode |
| `503` | Still loading, `--max-queue` is full, or `--queue-timeout` passed (both with `Retry-After`) | Wait and retry |
| `500` | The engine failed the request | Retry once; keep the stderr tail if it repeats |

A streaming request that fails after the stream has started gets one `data: {"error": {...}}`
frame instead of an HTTP status. Watch for it.
