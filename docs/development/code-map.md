[Documentation](../README.md) › [Development](README.md) › Code map

# Code map

Where the code lives and how one request moves through it. The [architecture notes](../architecture/README.md#source-layout)
explain each mechanism in depth; this page is the short version for someone about to change
something.

## The files

| File | Lines | What it owns | Change it when |
|---|---|---|---|
| `src/main.cpp` | ~80 | Startup order: parse, init the backend, then `onyx_server_run` with a load step | You change what happens before the first request |
| `src/args.h`, `src/args.cpp` | ~300 | `onyx_args`, the parser, `--help` and `--version` | You add or change a flag (both files, same change) |
| `src/server.h`, `src/server.cpp` | ~1500 | Every route, request parsing, response and SSE frames, the pre-routing checks (Origin, Host, key, loading), `/metrics` text, bind and listen | You add a field, a route or a response shape |
| `src/http_util.h`, `src/http_util.cpp` | ~200 | Pure request helpers: base64, media payloads, `X-Request-Id`, key compare, `Host` names, JSON nesting depth | The helper needs no engine; unit-test it |
| `src/llama_util.h`, `src/llama_util.cpp` | ~150 | Tokenize, detokenize, token-to-piece and `llama_batch` helpers on `llama.h`, the memory-capability probe (`onyx_probe_seq_rm`), and the quiet log filter | You need a llama.cpp helper that `common/` would otherwise provide |
| `src/onyx_chat.h`, `src/onyx_chat.cpp` | ~350 | The chat seam: template rendering, the chat grammar, output parsing and raw Jinja, over `common/chat.h` (the only includer) | You touch chat templates, tool calls or how output is parsed |
| `src/onyx_json.h` | ~20 | `onyx_json` (`nlohmann::ordered_json` from `third_party/nlohmann`) and `onyx_json_dump_safe` | You need JSON in Onyx code |
| `src/request.h`, `src/request.cpp` | ~100 | Request-body helpers: `stop`, max tokens, sampling fields, embeddings `dimensions` truncation | You change how a request field becomes a generation parameter; unit-test it |
| `src/sampler.h`, `src/sampler.cpp` | ~300 | `onyx_sampler`: the sampler chain and the grammar (rejection sampling, lazy triggers, generation-prompt prefill) on `llama.h` | You change how a token is picked or constrained |
| `src/engine.h`, `src/engine.cpp` | ~1900 | `onyx_engine`: model and context, the batching loop, slots, sampling, prefix reuse, context shift, reasoning budget, logprobs, embeddings, metrics counters | You change how tokens are scheduled, decoded or sampled |
| `src/decision.h`, `src/decision.cpp` | ~1900 | Decision models: family detection, request parsing, prompt rendering, scoring | You touch `/v1/systemone` |
| `src/convert.h`, `src/convert.cpp` | ~400 | `onyx_resolve_model`: GGUF passthrough, safetensors detection, running and caching the converter | You change what `-m` accepts |
| `scripts/convert-safetensors.py` | | The numpy-only converter | You widen safetensors support |
| `tests/unit/` | | The C++ unit tests (`check.h` is the whole harness) | Any change to `args`, `http_util`, `convert`, `request` or the sampler's trigger patterns |
| `scripts/smoke-test.sh` | | The end-to-end suite against tiny generated models | Any behaviour visible over HTTP or on the command line |
| `CMakeLists.txt`, `cmake/fetch-llama.cmake` | | Build, llama.cpp resolution and fetch, test registration | You add a source file or build option |

## One request, through the code

A `POST /v1/chat/completions` with `"stream": true`:

1. **`server.cpp`, pre-routing handler.** Echo `X-Request-Id`; refuse `Origin` (`403`), a
   non-loopback `Host` on a loopback bind (`403`), a wrong key (`401`), or any request while the
   model loads (`503`).
2. **`server.cpp`, the chat handler.** Parse the body (`parse_body`: the nesting-depth cap, then `onyx_json`, which is `nlohmann::ordered_json`), refuse unsupported fields
   (`reject_unsupported_fields`), decode media parts (`extract_media` → `http_util.cpp`), render
   the chat template (`onyx_chat_templates::render` → `onyx_chat.cpp`), build sampling and grammar
   (`onyx_parse_sampling` → `request.cpp`, `onyx_chat_render::apply_sampling`) and the reasoning budget, tokenize, check the prompt fits
   one slot, check `--max-queue`, then start the SSE stream.
3. **`engine.cpp`, `generate()`.** Builds the sampler on the HTTP thread (a bad grammar fails
   here), refuses if the queue is full, queues the request and waits on its own piece queue.
4. **`engine.cpp`, the loop thread.** `admit_queued()` gives the request a slot (the idle slot
   with the longest shared prefix), `build_batch()` adds decode rows first and prompt chunks after
   (bounded by `--tick-tokens`), `decode_batch()` runs `llama_decode` and calls `sample_slot()`
   per slot, and `emit()` pushes text to the request. `finish()` releases the slot and records
   the metrics.
5. **`server.cpp`, back on the HTTP thread.** The token callback turns each piece into a parsed
   delta (`onyx_chat_parser::update`) and writes `data:` frames; the end writes the
   finish frame, the `usage` and `timings` frame and `[DONE]`.

A non-streaming request takes the same path and builds one JSON response at the end. Embedding
and decision requests skip the loop and run on a mutex (`embed()`, `decide()`).

## Rules that keep it working

- **The parser and `--help` move together.** A flag in one is in the other. Supervisors probe
  `--help`.
- **Refuse, don't ignore.** An unsupported request field is a `400`, never silently dropped.
- **Nothing on a request's behalf touches the network or the filesystem.**
- **The engine loop never blocks on a client.** Text goes to a per-request queue; the HTTP thread
  writes it.
- **Anything on the [stability page](../reference/stability.md) changes only by its rule.**
