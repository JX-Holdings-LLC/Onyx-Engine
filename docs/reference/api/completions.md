[Documentation](../../README.md) › [Reference](../README.md) › [API reference](README.md) › Text completions

# Text completions

`POST /v1/completions`: raw prompt in, text out. No chat template is applied, so `reasoning_budget_tokens` is ignored here.

Requires `prompt` (string, or array of integer token ids); missing it is
`400`. Returns `501` if `--embedding` was passed.

Fields read: `prompt`, `stream`, `max_completion_tokens`/`max_tokens`/`n_predict`,
`stop`, the same sampling fields as chat completions, and directly (not via
the chat-template path) `grammar` (GBNF string) or `json_schema` (converted
with `json_schema_to_grammar`). Also `logprobs` (int `0..20`, the legacy
OpenAI form — presence enables it, the value is how many alternatives to
report per token; `400` outside that range).
`n` must be `1`. `echo`, `suffix`, `best_of`, `logit_bias`, and unsupported
sampler fields return `400`.

When `logprobs` is set, `choices[0].logprobs` uses the **legacy** flat
parallel-array OpenAI shape (deliberately, not the chat nested shape; llama.cpp itself does not
produce this shape correctly for `/v1/completions`):

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

With `json_schema`, a completion cut by `n_predict` or the context limit
answers `400` instead of returning incomplete JSON, exactly as on chat
completions; a stream ends on the `400` error frame.
