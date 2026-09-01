# API Reference

All request/response bodies are JSON. This document describes exactly what
`src/server.cpp` parses and returns — not the full OpenAI API surface, only
the subset `jx-engine` implements.

## Authentication

If `--api-key KEY` was passed, every route except `GET /health` (and
`OPTIONS` preflight requests) requires header:

```
Authorization: Bearer KEY
```

A missing or wrong value returns `401` with the standard error shape (below),
`type: "authentication_error"`. If `--api-key` is not set, no auth is
enforced anywhere.

## Error shape

Every error response (auth failures, bad requests, internal errors, and the
`501`s for endpoints disabled by mode) has the same JSON shape:

```json
{
  "error": {
    "message": "human-readable description",
    "type": "invalid_request_error | authentication_error | server_error | not_supported_error",
    "code": 400
  }
}
```

`code` mirrors the HTTP status. Unhandled C++ exceptions are caught by a
global exception handler and reported as `500`/`server_error` with the
exception's `what()` as `message`.

## `GET /health`

Always returns `200` with:

```json
{"status":"ok"}
```

Exempt from `--api-key` auth. This is a liveness check only — it does not
report whether the model is fully warmed up beyond having completed
`jx_engine::load` (the server does not start listening until load finishes).

## `GET /props`

Model and build metadata. No request body.

```json
{
  "model_alias": "my-model",
  "chat_template": "<jinja source or empty string>",
  "build_info": "jx-engine/0.1.0 (llama.cpp <commit>)",
  "n_ctx": 4096,
  "n_ctx_train": 32768,
  "n_embd": 4096,
  "embedding_mode": false,
  "model_desc": "<llama_model_desc() string>",
  "model_size_bytes": 4661211648,
  "model_n_params": 8030261248,
  "modalities": {"vision": false, "audio": false},
  "default_generation_settings": {"n_ctx": 4096}
}
```

Notes:
- `n_ctx` appears both top-level and nested under
  `default_generation_settings` — both are the same value
  (`jx_engine::n_ctx()`, the actual context size the running context was
  created with, from `llama_n_ctx`).
- `modalities.vision`/`modalities.audio` are hardcoded `false` in v1
  (multimodal is not implemented — see the README roadmap).
- `chat_template` is the chat template source llama.cpp resolved
  (`common_chat_templates_source`), which can be an empty string if the
  model carries no template and none was overridden.

## `GET /v1/models`

```json
{
  "object": "list",
  "data": [
    {
      "id": "my-model",
      "object": "model",
      "created": 1735689600,
      "owned_by": "jx-engine",
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

Always exactly one entry — `jx-engine` serves one model per process. `id` is
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

Renders a chat prompt from `messages` without generating anything. Same
request-body fields as `/v1/chat/completions` (`messages`, `tools`,
`tool_choice`, `parallel_tool_calls`, `grammar`, `json_schema`,
`response_format`, `chat_template_kwargs` — see `parse_chat_inputs` in
`server.cpp`), but nothing is tokenized or decoded.

Request requires `messages`; missing it returns `400`.

Response:

```json
{"prompt": "<rendered prompt text>"}
```

## `POST /v1/chat/completions`

Requires `messages` (array); missing/wrong type returns `400`. Returns `501`
if the process was started with `--embedding`.

**Fields read from the request body:**

| Field | Notes |
|---|---|
| `messages` | required; parsed via `common_chat_msgs_parse_oaicompat` |
| `stream` | bool, default `false` |
| `tools` | array; parsed if non-empty |
| `tool_choice` | string only (`common_chat_tool_choice_parse_oaicompat`) |
| `parallel_tool_calls` | bool |
| `grammar` | string; GBNF grammar, used as `COMMON_GRAMMAR_TYPE_USER` |
| `json_schema` | object; used as `COMMON_GRAMMAR_TYPE_OUTPUT_FORMAT` |
| `response_format` | `{"type":"json_object"[,"schema":...]}` or `{"type":"json_schema","json_schema":{"schema":...}}` or `{"type":"text"}`; any other `type` is a `400` |
| `chat_template_kwargs` | object; passed through to the jinja template |
| `max_completion_tokens`, then `max_tokens`, then `n_predict` | first present, non-null one wins; default `-1` (until EOG or context limit) |
| `stop` | string or array of strings; appended to any stop sequences the chat template itself requested |
| `temperature`, `top_p`, `top_k`, `min_p`, `seed`, `repeat_penalty`, `repeat_last_n`, `presence_penalty`, `frequency_penalty` | sampling params — see `parse_sampling` |
| `logprobs` | bool, default `false`; when `true`, report per-token logprobs (see below) |
| `top_logprobs` | int `0..20`; how many alternative tokens to report per position; requires `logprobs: true`, else `400` |
| `reasoning_budget_tokens` | int; per-request override of `--reasoning-budget` (see below); ignored on `/v1/completions` (no chat template there) |

**Logprobs.** When `logprobs: true`, `choices[0].logprobs` is:

```json
{
  "content": [
    {
      "token": " Hi",
      "logprob": -0.234,
      "bytes": [32, 72, 105],
      "top_logprobs": [
        {"token": " Hi", "logprob": -0.234, "bytes": [32, 72, 105]},
        {"token": " Hello", "logprob": -1.9, "bytes": [32, 72, 101, 108, 108, 111]}
      ]
    }
  ]
}
```

One entry per generated token whose text survived into the response (a token
trimmed off by a stop sequence gets no entry). `logprob` is the **raw** model
logprob (softmax over the full vocabulary from that token's logits row,
computed before any sampler-chain transform), matching OpenAI semantics: a
grammar- or constraint-picked token can legitimately show a very low raw
logprob. `top_logprobs` has exactly `top_logprobs` entries (0 if the request
field was absent/`0`), sorted by `logprob` descending; it does not
necessarily include the sampled token itself. `null` when `logprobs` was not
requested. Streaming: each SSE chunk that delivers newly-completed token text
carries `choices[0].logprobs.content[]` for just those tokens; a chunk with
no new completed tokens carries `logprobs: null`. Because logprobs are keyed
to raw generated tokens while `delta.content`/`delta.tool_calls` are diffs of
the *parsed* message, the two are not aligned frame-for-frame — a logprobs
frame may carry an empty `delta: {}`.

**Reasoning budget** (`--reasoning-budget`/`--reasoning-budget-message`, or
the per-request `reasoning_budget_tokens` override). `-1` (default):
unrestricted, feature inert. `0`: suppress thinking entirely — the model's
thinking-open tag (from the chat template's own `thinking_start_tag`, or
`<think>`/`</think>` if the template exposes none) is closed on the very
first generated token, no message injected. `N > 0`: up to `N` generated
tokens are allowed inside the thinking block; the token that would be the
`(N+1)`th is replaced by the forced sequence — `--reasoning-budget-message`
text (if set) followed by the closing tag — emitted token-by-token as normal
generated text (the response's reasoning/content split, if any, is still
whatever `common_chat_parse` derives from that text; jx-engine does not
special-case it). A template whose *rendered generation prompt* already ends
inside the thinking block (deepseek-style) starts already in the
budget-counting state.

`grammar`/`json_schema`/`response_format`/`tools` are mutually reinforcing,
not independent: the chat template decides the actual grammar type applied
(`COMMON_GRAMMAR_TYPE_USER` for a caller `grammar`, `_OUTPUT_FORMAT` for
`json_schema`/`response_format`, otherwise `_TOOL_CALLS` so template-driven
tool-call grammar can apply).

**Non-streaming response:**

```json
{
  "id": "chatcmpl-<random hex>",
  "object": "chat.completion",
  "created": 1735689600,
  "model": "my-model",
  "choices": [
    {
      "index": 0,
      "message": {"role": "assistant", "content": "...", "tool_calls": [...]},
      "finish_reason": "stop | length | tool_calls"
    }
  ],
  "usage": {"prompt_tokens": 12, "completion_tokens": 34, "total_tokens": 46},
  "timings": {
    "cache_n": 8, "prompt_n": 4, "prompt_ms": 12.3, "prompt_per_second": 325.2,
    "predicted_n": 34, "predicted_ms": 890.1, "predicted_per_second": 38.2
  }
}
```

`finish_reason` is `"length"` if the response hit `n_predict`/context limit,
otherwise `"tool_calls"` if the parsed message contains tool calls, otherwise
`"stop"`.

**Streaming (`"stream": true`).** Server-Sent Events, `Content-Type:
text/event-stream`. Each frame is `data: <json>\n\n`:

1. One role-announcement chunk: `delta: {"role":"assistant","content":""}`.
2. Zero or more delta chunks as generation proceeds, each an OpenAI-style
   `chat.completion.chunk` whose `choices[0].delta` carries whichever of
   `reasoning_content`, `content`, or `tool_calls` changed since the last
   diff (`common_chat_msg_diff`). Deltas that would be empty are not sent.
3. One finish chunk: `choices[0].delta` is `{}`, `choices[0].finish_reason`
   set to `"stop"`/`"length"`/`"tool_calls"`.
4. One final usage frame — same `id`/`object`/`created`/`model`, but
   `"choices": []` (empty) and top-level `usage` and `timings` objects (same
   shape as the non-streaming response).
5. A literal `data: [DONE]\n\n`.

If generation errors mid-stream, one `data: {"error":{"message":...,"type":"server_error"}}`
frame is sent and the stream ends (no `[DONE]`). If the client disconnects,
the stream simply stops (no further frames).

## `POST /v1/completions`

Requires `prompt` (string, or array of integer token ids); missing it is
`400`. Returns `501` if `--embedding` was passed.

Fields read: `prompt`, `stream`, `max_completion_tokens`/`max_tokens`/`n_predict`,
`stop`, the same sampling fields as chat completions, and directly (not via
the chat-template path) `grammar` (GBNF string) or `json_schema` (converted
with `json_schema_to_grammar`). Also `logprobs` (int `0..20`, the legacy
OpenAI form — presence enables it, the value is how many alternatives to
report per token; `400` outside that range).

When `logprobs` is set, `choices[0].logprobs` uses the **legacy** flat
parallel-array OpenAI shape (deliberately, not the chat nested shape —
llama.cpp itself never implemented this shape correctly for
`/v1/completions`, see the v2 API reference):

```json
{
  "tokens": [" Once", " upon"],
  "token_logprobs": [-0.5, -1.2],
  "top_logprobs": [{" Once": -0.5, " The": -2.1}, {" upon": -1.2, " a": -1.8}],
  "text_offset": [0, 5]
}
```

`text_offset[i]` is the byte offset of `tokens[i]` from the start of the
*returned* completion text (`choices[0].text`), not the prompt. Streaming:
each frame carries its own tokens' worth of these same four arrays, with
`text_offset` continuing to accumulate across frames from the start of the
whole completion.

Non-streaming response (`object: "text_completion"`):

```json
{
  "id": "cmpl-<random hex>",
  "object": "text_completion",
  "created": 1735689600,
  "model": "my-model",
  "choices": [{"index": 0, "text": "...", "finish_reason": "stop | length"}],
  "usage": {"prompt_tokens": 12, "completion_tokens": 34, "total_tokens": 46},
  "timings": {"...": "same shape as chat completions"}
}
```

Streaming: one `data: {...}` frame per non-empty generated piece
(`choices[0].text` = that piece, `finish_reason: null`), then a final frame
with `choices[0].text: ""`, the real `finish_reason`, plus `usage`/`timings`,
then `data: [DONE]\n\n`. Same mid-stream error frame behavior as chat
completions.

## `POST /v1/embeddings`

Returns `501` unless the process was started with `--embedding`. Requires
`input`; missing it is `400`.

`input` accepts: a string, an array of strings, an array of integer token
ids (a single pre-tokenized input), or an array of arrays of integers
(multiple pre-tokenized inputs). Mixed-type arrays (some string items, some
non-string/non-array items) return `400`.

Response:

```json
{
  "object": "list",
  "model": "my-model",
  "data": [
    {"object": "embedding", "index": 0, "embedding": [0.0123, -0.045, ...]}
  ],
  "usage": {"prompt_tokens": 12, "total_tokens": 12}
}
```

Embeddings are L2-normalized. `usage.completion_tokens` is not reported
(embeddings have none); `prompt_tokens`/`total_tokens` are the summed input
token counts across all items in the batch.

## CORS and misc headers

Every response carries `Access-Control-Allow-Origin: *`,
`Access-Control-Allow-Headers: Authorization, Content-Type`,
`Access-Control-Allow-Methods: GET, POST, OPTIONS`, and
`Server: jx-engine/<version>`. `OPTIONS` on any path returns `204`.
Read/write socket timeouts are 600 seconds.
