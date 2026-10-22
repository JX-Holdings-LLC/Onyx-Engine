[Documentation](../../README.md) › [Reference](../README.md) › [API reference](README.md) › Chat completions

# Chat completions

`POST /v1/chat/completions`: streaming or not, with tools, structured output, logprobs, a reasoning budget and (with `--mmproj`) image and audio parts. For a worked tour of the common cases see [structured output and tools](../../guides/structured-output-and-tools.md) and [multimodal input](../../guides/multimodal.md). How a request becomes tokens is in [one request, start to finish](../../architecture/concurrency.md#one-request-start-to-finish).

Requires `messages` (array); missing/wrong type returns `400`. Returns `501`
if the process was started with `--embedding`.

## Multimodal content parts

A message's `content` may be an array whose
parts include `image_url` (and `input_audio`, if the loaded `--mmproj`
projector supports audio) alongside `text`:

```json
{"type": "image_url", "image_url": {"url": "data:image/png;base64,iVBORw0KG..."}}
{"type": "image_url", "image_url": "iVBORw0KG..."}
{"type": "input_audio", "input_audio": {"data": "UklGRg==...", "format": "wav"}}
```

`image_url.url` (or the bare string form) and `input_audio.data` accept
only a `data:<mime>;base64,...` URI or a raw base64 string — `http://`,
`https://`, and `file://` are all rejected with `400`, since onyx-engine
performs no network or filesystem I/O on a request's behalf; inline the
bytes instead. Before the chat template renders, each such part is decoded
and replaced by an internal media marker so mtmd tokenizes the rendered text
and matches markers to decoded buffers in order; the media itself is
prefilled at slot admission (see
[the architecture note](../../architecture/multimodal.md)). Requests fail with
`400` when:

- a media part carries no data, or a data: URI is malformed or not base64;
- the server was not started with `--mmproj` at all;
- an `image_url` part is sent to a projector without vision support, or an
  `input_audio` part to one without audio support (`GET /props`'s
  `modalities` reports which are available);
- the part's URL uses a rejected scheme (`http://`/`https://`/`file://`).

## Fields read from the request body


| Field | Notes |
|---|---|
| `messages` | required; parsed via `common_chat_msgs_parse_oaicompat`; `content` may include `image_url`/`input_audio` parts (above) |
| `stream` | bool, default `false` |
| `tools` | array; parsed if non-empty |
| `tool_choice` | `auto`, `none`, `required`, or a function object naming one provided tool |
| `n` | only `1` is supported; other values return `400` |
| `stream_options.include_usage` | `false` omits the final usage frame |
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

`model` and `user` are accepted as client metadata; this single-model server
always uses the loaded model alias. Unsupported sampler fields such as
`typical_p`, `mirostat*`, `dynatemp*`, `dry_*`, `xtc_*`, and `logit_bias`
return `400` rather than being silently ignored.

## Logprobs

When `logprobs: true`, `choices[0].logprobs` is:

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
requested. Streaming logprobs arrive in separate SSE frames with an empty
`delta: {}` before the corresponding text delta; text and tool-call delta
frames carry `logprobs: null`.

## Reasoning budget

Set with `--reasoning-budget` and `--reasoning-budget-message`, or per request with
`reasoning_budget_tokens`. `-1` (default):
unrestricted, feature inert. `0`: suppress thinking entirely — the model's
thinking-open tag (from the chat template's own `thinking_start_tag`, or
`<think>`/`</think>` if the template exposes none) is closed on the very
first generated token, no message injected. `N > 0`: up to `N` generated
tokens are allowed inside the thinking block; the token that would be the
`(N+1)`th is replaced by the forced sequence — `--reasoning-budget-message`
text (if set) followed by the closing tag — emitted token-by-token as normal
generated text (the response's reasoning/content split, if any, is still
whatever `common_chat_parse` derives from that text; onyx-engine does not
special-case it). A template whose *rendered generation prompt* already ends
inside the thinking block (deepseek-style) starts already in the
budget-counting state.

`grammar`/`json_schema`/`response_format`/`tools` are mutually reinforcing,
not independent: the chat template decides the actual grammar type applied
(`COMMON_GRAMMAR_TYPE_USER` for a caller `grammar`, `_OUTPUT_FORMAT` for
`json_schema`/`response_format`, otherwise `_TOOL_CALLS` so template-driven
tool-call grammar can apply).

## Non-streaming response


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

Generation responses include `truncated: true` when `--context-shift` discarded older tokens. Streaming responses report it on their final usage frame.

## Streaming

With `"stream": true` the response is Server-Sent Events, `Content-Type:
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
