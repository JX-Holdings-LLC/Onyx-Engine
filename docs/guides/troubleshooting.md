[Documentation](../README.md) › [Guides](README.md) › Troubleshooting

# Troubleshooting

Find the message, read the cause. Messages are quoted as the engine prints them.

## It will not start

| You see | Cause and fix |
|---|---|
| `error: --model is required (see --help)` | No `-m`. |
| `error: invalid --pooling value 'x'` | Use `none`, `mean`, `cls`, `last` or `rank`. |
| `error: unknown argument '--foo' (see --help)` | The flag is not implemented. The parser accepts only what `--help` lists; see [the CLI reference](../reference/cli.md). |
| `error: -fa requires a value` | `-fa` needs `on`, `off`, `auto`, `1` or `0`. |
| `error: --pooling requires --embedding` | Add `--embedding`. |
| `error: failed to bind to HOST:PORT` | The port is in use, or the address is not on this machine. |
| `error: model path does not exist: '...'` | The `-m` path is wrong. Relative paths resolve against the directory you launched from. |
| `error: failed to load model from '...'` | The file is missing, truncated, or not a model llama.cpp can read. Check the path and the file's size. |
| `error: failed to create inference context (ctx-size N)` | Not enough memory for that context. Lower `-c`, or offload fewer layers. |
| `error: failed to load multimodal projector from '...' (--mmproj)` | The projector does not match the model, or the path is wrong. |
| `error: --embedding cannot be used with a decision model` | A decision model serves `/v1/systemone` only. |
| `error: --chat-template expects Jinja source or 'chatml'` | You passed a path. Use `--chat-template-file`. |
| a converter error naming a missing script | Set `ONYX_ENGINE_CONVERT_SCRIPT`, or run the binary from where `scripts/` sits beside it. |
| a converter refusal naming an architecture, tokenizer or dtype | The safetensors model is outside what the converter handles. Use a GGUF. |

The port is not open until loading finishes. A refused connection in the first seconds is normal on a big model; poll `GET /health` rather than sleeping.

## A request failed

| Status and message | Cause and fix |
|---|---|
| `401 invalid or missing API key` | The server was started with `--api-key`. Send `Authorization: Bearer KEY`. |
| `403 browser-origin requests are disabled` | The request carried an `Origin` header. Call from a server, not a page; or let a proxy strip it. |
| `400 unsupported field: logit_bias` (or `mirostat`, `dry_*`, `xtc_*`, `typical_p`, `dynatemp_*`) | Not implemented. Remove the field. See [compatibility](../reference/compatibility.md). |
| `400 only n=1 is supported` | Send one request per choice. |
| `400 prompt does not fit in the context window` | The prompt is longer than **one slot's** window. Read `n_ctx` from `/props`, shorten the prompt, or raise `-c` (or lower `-np`). |
| `400 top_logprobs requires logprobs to be set to true` | Set `logprobs: true`. |
| `400 failed to apply chat template: ...` | The template rejected your `messages` (roles out of order, for example). Reproduce with `POST /apply-template`. |
| `400 reasoning budget cannot force tokens through a non-lazy grammar` | A grammar and a reasoning budget conflict. Drop one. |
| `400 dimensions is not supported` | Embeddings do not truncate. Do not send `dimensions`. |
| `501 this instance serves embeddings only` | The process was started with `--embedding`. Use another process for chat. |
| `501 embeddings are disabled; start onyx-engine with --embedding` | The reverse. |
| `501 this instance serves a decision model; use /v1/systemone` | The process holds a decision model. |
| `501 this model is not a decision model` | `/v1/systemone` needs a model whose metadata carries `<arch>.decision.type`. |
| `500` with a message | An internal error. Run with `-v`, keep the log, and file an issue with the request that triggers it. |

## It is slow, or it stalls

- **First request slow.** The model is mapped lazily; the first prompt pays for it. Run a warm-up request after starting.
- **Time to first token is long on every call.** The prompt is being decoded from scratch. Check `timings.cache_n` in the response: zero means no prefix was reused. Keep the stable text at the *front* of the prompt, and keep `--cache-reuse` above 0. A `--mmproj` projector turns reuse off.
- **Other streams hiccup when an image arrives.** That is the media prefill holding the engine loop. See [multimodal input](multimodal.md#what-it-costs). Use a dedicated process for image traffic.
- **Requests queue even though load is light.** `-np 1` is the default. Raise it, and raise `-c` with it.
- **Generation stops with `finish_reason: "length"`.** The request hit `max_tokens` or its slot's context. Raise either, or enable `--context-shift`.
- **GPU idle, CPU pegged.** The binary was built without a GPU backend, or `-ngl 0` was passed. Release binaries are CPU-only; build with a backend.

## The answers look wrong

- **Odd formatting or runaway turns.** The wrong chat template. Compare `/props` `chat_template` with the model's card, then override it with `--chat-template-file`.
- **A control token in a message is treated as plain text.** That is deliberate: only the chat template can produce special tokens. If you need one, it belongs in the template, not in message content.
- **JSON is cut off.** The generation hit its length limit; the JSON is incomplete, and `finish_reason` says `"length"`. See [structured output](structured-output-and-tools.md#a-json-schema).
- **Logprobs are very low for a token the model picked.** They are the raw distribution, before any grammar or sampler. That is intended, and matches OpenAI semantics.

## Still stuck

Run once with `-v` to get llama.cpp's own log, reproduce with the smallest request, and open an issue with the command line, the log, `GET /props`, and the request. The smoke test is the quickest way to tell a broken build from a broken request: `npm test`.
