[Documentation](../../README.md) › [Reference](../README.md) › [API reference](README.md) › Service endpoints

# Service endpoints

The probes and text utilities. They answer in every mode (text, `--embedding` and decision model), and `GET /health` is exempt from `--api-key`.

## `GET /health`

Exempt from `--api-key`. The answer depends on whether the model has loaded:

| State | Status | Body |
|---|---|---|
| Loading (or converting a safetensors model) | `503` | `{"error":{"code":503,"message":"Loading model","type":"unavailable_error"}}` |
| Ready | `200` | `{"status":"ok"}` |

The server binds as soon as it has parsed its arguments and started its backends, before it loads
the model, so a refused connection means the port is not open yet (while the process is alive) or
the process has exited, never "still loading". A load that fails
closes the port and the process exits 1. The loading body is llama-server's, so a supervisor
written for llama-server reads it unchanged. Every other route answers `503` with the same body
while loading (after the `--api-key` check). See [supervising](../../guides/supervising.md).

## `GET /props`

Model and build metadata. No request body.

```json
{
  "model_alias": "my-model",
  "chat_template": "<resolved Jinja source>",
  "build_info": "onyx-engine/<version> (llama.cpp b11820-d77256999)",
  "port": 8080,
  "n_ctx": 4096,
  "n_ctx_total": 4096,
  "n_ctx_train": 32768,
  "n_embd": 4096,
  "embedding_mode": false,
  "decision_type": null,
  "model_desc": "<llama_model_desc() string>",
  "model_size_bytes": 4661211648,
  "model_n_params": 8030261248,
  "modalities": {"vision": false, "audio": false},
  "memory_seq_rm": "partial",
  "default_generation_settings": {"n_ctx": 4096}
}
```

Notes:
- **`n_ctx` is the PER-SLOT context** — the window one request is measured
  against (`onyx_engine::n_ctx_slot()`, from `llama_n_ctx_seq`). It appears
  both top-level and nested under `default_generation_settings`, and the two
  are always the same number, so a client may read either.

  This is llama-server parity: llama-server's `get_res_props()` publishes
  `default_generation_settings.n_ctx` as `meta.slot_n_ctx`, which is its own
  `n_ctx_slot()`. It matters because `--parallel N` divides the context —
  llama.cpp gives each of the `N` slots roughly `n_ctx / N` tokens — so with
  `-c 512 --parallel 2` a request is refused above 256 tokens, and reporting
  the undivided 512 would tell a caller it had twice the window it really
  has. With the default `--parallel 1` the two values coincide.
- `n_ctx_total` is the whole context the llama.cpp context was created with
  (`onyx_engine::n_ctx()`, from `llama_n_ctx`) — that is, `n_ctx` times the
  slot count. It has no llama-server counterpart and is informational; size
  requests against `n_ctx`, not this.
- `modalities.vision`/`modalities.audio` reflect the loaded `--mmproj`
  projector's real capabilities (`mtmd_support_vision`/`mtmd_support_audio`)
  — both `false` when no projector is loaded.
- `memory_seq_rm` is what the context's memory can cut back, probed once at load by
  decoding two tokens and removing the second: `"partial"` (a KV cache: any shared prefix
  can be reused), `"full"` (recurrent state without rollback: only a prompt that extends a
  slot's cached tokens exactly is reused), `"bounded"` (recurrent rollback, up to
  `llama_n_rs_seq` tokens) or `"none"`. It is `null` in embedding and decision mode, which
  run no batching loop. Informational: prefix reuse already adapts to it.
- `chat_template` is the resolved chat template source. Models without one
  use the ChatML fallback.
- `port` is the port actually bound. It differs from `--port` only when that was `0`.
- `decision_type` names the decision-model family (`"openjev"`, `"laya"`,
  ...) when the loaded model is one, and is `null` otherwise. A decision
  model serves [`POST /v1/systemone`](systemone.md) only.

## `GET /metrics`

Only with `--metrics` (otherwise `404`), and behind `--api-key` like any other route. Prometheus
text exposition format (`text/plain; version=0.0.4`), using llama-server's `llamacpp:*` names so
dashboards built for it read Onyx unchanged:

| Series | Type | Meaning |
|---|---|---|
| `llamacpp:prompt_tokens_total` | counter | Prompt tokens evaluated (prefix-cache hits excluded) |
| `llamacpp:prompt_seconds_total` | counter | Time spent on prompts |
| `llamacpp:tokens_predicted_total` | counter | Tokens generated |
| `llamacpp:tokens_predicted_seconds_total` | counter | Time spent generating |
| `llamacpp:n_decode_total` | counter | `llama_decode()` calls made by the batching loop |
| `llamacpp:n_tokens_max` | counter | Largest KV fill a slot has reached |
| `llamacpp:n_busy_slots_per_decode` | counter | Average slots in use per decode |
| `llamacpp:prompt_tokens_seconds` | gauge | Average prompt throughput, tokens per second |
| `llamacpp:predicted_tokens_seconds` | gauge | Average generation throughput, tokens per second |
| `llamacpp:requests_processing` | gauge | Requests holding a slot now |
| `llamacpp:requests_deferred` | gauge | Requests waiting for a slot now |
| `onyx:requests_rejected_total` | counter | Requests refused with `503` because `--max-queue` was full |
| `onyx:requests_timed_out_total` | counter | Requests answered `503` after waiting `--queue-timeout` seconds for a slot |

The counters cover chat and text completions since the process started. Embedding and decision
requests are not counted, and a multimodal prefill's own decode calls are not in `n_decode_total`.

## Headers on every route

- **`X-Request-Id`**: when a request carries one of up to 128 printable ASCII characters (no
  spaces), the response carries the same value, including on errors. Anything else is not
  echoed.
- **`Retry-After: 1`** accompanies a `503` caused by `--max-queue` or `--queue-timeout`.
- **`Server: onyx-engine/<version>`**.

## `GET /v1/models`

```json
{
  "object": "list",
  "data": [
    {
      "id": "my-model",
      "object": "model",
      "created": 1735689600,
      "owned_by": "onyx-engine",
      "meta": {
        "n_ctx_train": 32768,
        "n_embd": 4096,
        "size": 4661211648,
        "n_params": 8030261248
      }
    }
  ]
}
```

Always exactly one entry — `onyx-engine` serves one model per process. `id` is
the `--alias` value. `created` is the current wall-clock time at request
time (`std::time(nullptr)`), not the model file's mtime or any fixed value.

## `POST /tokenize`

Request:

```json
{"content": "some text", "add_special": false}
```

`content` defaults to `""`; `add_special` defaults to `false`. Tokenization
always uses `parse_special = true` (special/control tokens in the text are
parsed as tokens).

Response:

```json
{"tokens": [1, 2345, 678]}
```

## `POST /detokenize`

Request:

```json
{"tokens": [1, 2345, 678]}
```

`tokens` defaults to an empty array if absent or not an array. Response:

```json
{"content": "some text"}
```

Detokenization uses `special = false` (special tokens are not rendered into
the output text).

## `POST /apply-template`

Renders a chat prompt from `messages` without generating anything. Supports
the chat-template fields (`messages`, `tools`,
`tool_choice`, `parallel_tool_calls`, `grammar`, `json_schema`,
`response_format`, `chat_template_kwargs` — see `parse_chat_inputs` in
`server.cpp`), but does not process image or audio content parts. Nothing is
tokenized or decoded.

Request requires `messages`; missing it returns `400`.

Response:

```json
{"prompt": "<rendered prompt text>"}
```
