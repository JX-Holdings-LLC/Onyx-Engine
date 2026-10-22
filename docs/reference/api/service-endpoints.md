[Documentation](../../README.md) › [Reference](../README.md) › [API reference](README.md) › Service endpoints

# Service endpoints

The probes and text utilities. They answer in every mode (text, `--embedding` and decision model), and `GET /health` is exempt from `--api-key`.

## `GET /health`

Always returns `200` with:

```json
{"status":"ok"}
```

Exempt from `--api-key` auth. This is a liveness check only — it does not
report whether the model is fully warmed up beyond having completed
`onyx_engine::load` (the server does not start listening until load finishes).

## `GET /props`

Model and build metadata. No request body.

```json
{
  "model_alias": "my-model",
  "chat_template": "<resolved Jinja source>",
  "build_info": "onyx-engine/<version> (llama.cpp b11820-d77256999)",
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
- `chat_template` is the resolved chat template source. Models without one
  use the ChatML fallback.
- `decision_type` names the decision-model family (`"openjev"`, `"laya"`,
  ...) when the loaded model is one, and is `null` otherwise. A decision
  model serves [`POST /v1/systemone`](systemone.md) only.

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
