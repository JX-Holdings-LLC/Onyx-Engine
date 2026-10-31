[Documentation](../README.md) › [Guides](README.md) › Using it with other clients

# Using it with other clients

The job: point an OpenAI-compatible client at Onyx Engine. The base URL is
`http://127.0.0.1:<port>/v1`. The model name is whatever you passed as `--alias`; `GET /v1/models`
lists exactly that one. Onyx ignores the model name in requests, because a process serves exactly
one model. If you started the engine with `--api-key KEY`, give the client that key. If not, give
it any non-empty string, since most clients insist on one.

## What strict field handling means for a client

Onyx refuses what it does not implement instead of quietly ignoring it. A client that sends one of
these fields gets `400 unsupported field: <name>`:

- `logit_bias`, `typical_p`, `mirostat*`, `dynatemp_*`, `dry_*`, `xtc_*` on chat and completions;
- `echo`, `suffix`, `best_of` on completions;
- `n` other than 1.

`dimensions` on embeddings is accepted (truncate and renormalise); a value outside 1 to
the model's `n_embd` is a `400`.

Most clients send none of these unless you set them. If a client returns a `400` naming a field,
turn that setting off in the client. The full list is in [compatibility and limits](../reference/compatibility.md).

## `openai` for Python

Checked against `openai` 3.36 with chat (plain, streamed, logprobs, `json_schema`), completions
and embeddings.

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="unused")

r = client.chat.completions.create(
    model="my-model",
    messages=[{"role": "user", "content": "Hello!"}],
    max_tokens=64,
)
print(r.choices[0].message.content)

for chunk in client.chat.completions.create(
        model="my-model", messages=[{"role": "user", "content": "Count to five."}],
        stream=True, stream_options={"include_usage": True}):
    if chunk.choices and chunk.choices[0].delta.content:
        print(chunk.choices[0].delta.content, end="", flush=True)
```

Embeddings need an engine started with `--embedding`. The client asks for base64 by default,
and Onyx serves it:

```python
emb = OpenAI(base_url="http://127.0.0.1:8081/v1", api_key="unused")
vectors = [d.embedding for d in emb.embeddings.create(model="my-embedder", input=["a", "b"]).data]
```

`logit_bias` raises `openai.BadRequestError` (400). Responses carry two extra fields the client
keeps but does not type: `timings` (prompt and generation speed) and `truncated`.

## `openai` for JavaScript and TypeScript

Checked against `openai` on npm with chat, streaming and embeddings.

```js
import OpenAI from 'openai';

const client = new OpenAI({ baseURL: 'http://127.0.0.1:8080/v1', apiKey: 'unused' });

const r = await client.chat.completions.create({
  model: 'my-model',
  messages: [{ role: 'user', content: 'Hello!' }],
  max_tokens: 64,
});
console.log(r.choices[0].message.content);

const stream = await client.chat.completions.create({
  model: 'my-model', messages: [{ role: 'user', content: 'Count to five.' }], stream: true,
});
for await (const chunk of stream) process.stdout.write(chunk.choices[0]?.delta?.content ?? '');
```

## curl

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello!"}],"max_tokens":64}'
```

Add `"stream": true` and `curl -N` to stream. The last data frame before `[DONE]` carries `usage`
and `timings`.

## Open WebUI

Add an OpenAI-compatible connection (**Admin settings → Connections**, or `OPENAI_API_BASE_URL`)
with the base URL above and the key. The engine's one model appears under its alias.

Not tested by this project. Two things to know:

- **Open WebUI in Docker cannot reach a loopback-bound engine by name.** It connects as
  `host.docker.internal` (or a container name), and a loopback-bound Onyx refuses any `Host` that
  is not a loopback name with `403` (the [DNS-rebinding guard](security.md#what-the-engine-does)).
  Bind the engine to an address the container can reach and set `--api-key`, or run Open WebUI on
  the host.
- If a chat fails with `400 unsupported field`, clear that parameter in the model's advanced
  settings.

## Continue

In Continue's `config.yaml`:

```yaml
models:
  - name: Onyx
    provider: openai
    model: my-model
    apiBase: http://127.0.0.1:8080/v1
    apiKey: unused
```

Not tested by this project. Continue's chat and edit features use chat completions. Autocomplete
and fill-in-the-middle need a code model and its template; Onyx renders the model's own chat
template and has no separate fill-in-the-middle route.

## Clients that want something Onyx does not serve

| The client wants | Status |
|---|---|
| `/v1/responses` (newer OpenAI SDK features, Codex) | Not served; on the [roadmap](../../ROADMAP.md) |
| `/v1/messages` (Anthropic-style clients) | Not served; on the roadmap |
| Several models from one base URL | Run one engine per model behind a router, or use [JX Runtime](../integration/jx-runtime.md) |
| A browser calling the engine directly | Refused: any request with `Origin` gets `403`. Put a server in between. |
