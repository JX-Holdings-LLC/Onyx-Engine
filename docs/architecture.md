# Architecture

## Process model

`jx-engine` is a single-model, single-process server. Each process:

1. parses CLI arguments (`src/args.cpp`),
2. loads exactly one GGUF model and creates one `llama_context`
   (`jx_engine::load` in `src/engine.cpp`),
3. blocks in an HTTP server loop until the process is signalled
   (`jx_server_run` in `src/server.cpp`).

There is no in-process model swapping and no multi-model registry. Running
more than one model means running more than one `jx-engine` process, each on
its own port. This is a deliberate v1 simplification: the intended caller is
JX Runtime, which already manages one child process per loaded model (see
[`jx-runtime-integration.md`](jx-runtime-integration.md)), so `jx-engine`
does not duplicate that bookkeeping.

## Source layout

| File | Responsibility |
|---|---|
| `src/args.h` / `src/args.cpp` | `jx_args` struct, `argv` parsing, `--help`/`--version` text |
| `src/engine.h` / `src/engine.cpp` | `jx_engine`: owns the llama.cpp model + context, runs generation and embedding, tokenize/detokenize helpers |
| `src/server.h` / `src/server.cpp` | HTTP layer built on `cpp-httplib`: request parsing, response/SSE-frame construction, all route handlers |
| `src/main.cpp` | Entry point: parse args, init the llama.cpp backend, load the model, run the server, tear down |

`server.cpp` depends on `engine.h` (to call generation/embedding/tokenize)
and on llama.cpp's `common` library headers (`chat.h`, `common.h`,
`json.h`, `json-schema-to-grammar.h`, `sampling.h`, `log.h`) for chat
templating, JSON, grammar construction, and sampling parameter types.
`engine.cpp` depends directly on `llama.h` (the core llama.cpp C API) plus
`common.h`/`sampling.h`/`log.h`.

## Concurrency model

- **One mutex, one generation at a time.** `jx_engine` holds a single
  `std::mutex` (`jx_engine::mutex_`); both `generate()` and `embed()` take a
  `std::lock_guard` on it for their full duration. Whatever HTTP thread calls
  in, it blocks until the current generation finishes.
- **HTTP layer is multi-threaded.** `jx-engine` uses `cpp-httplib`'s
  (`httplib::Server`) built-in thread pool, so multiple requests can be
  in-flight in the HTTP layer (accepting connections, parsing bodies,
  building responses) — but they still serialize on the engine mutex the
  moment they call into `generate()`/`embed()`. `--parallel`/`-np` is parsed
  and stored (`jx_args::n_parallel`) but nothing in `engine.cpp` or
  `server.cpp` branches on it to run more than one generation concurrently;
  it exists for command-line compatibility with `llama-server` only.
- **Streaming is one SSE write callback, not a separate thread pool.**
  Chat/text completion streaming uses `httplib::Response::set_chunked_content_provider`.
  The provider lambda is called once (`offset == 0`); it does all of its work
  — including the entire blocking call into `engine.generate()` — inside that
  one invocation, writing SSE frames to the `httplib::DataSink` from within
  the token callback (`jx_token_cb`) as tokens are produced. There is no
  separate producer/consumer thread inside `jx-engine` for a single stream;
  the token callback *is* what writes each frame, synchronously, on the
  thread handling that request.
- **Cancellation.** The token callback returns `bool`; returning `false`
  (e.g. because `sink.write()` failed, meaning the client disconnected) sets
  `jx_gen_result::finish = JX_FINISH_CANCEL` and unwinds generation for that
  request. It does not affect other requests, which are already serialized
  behind the mutex regardless.

## KV-cache prefix reuse

`jx_engine::decode_prompt` (in `engine.cpp`) keeps a copy of the token
sequence currently materialized in the KV cache (`cache_tokens_`). On each
new prompt:

1. It compares the new prompt's tokens against `cache_tokens_`,
   token-by-token, to find the longest common prefix — capped so at least
   one token is always left to evaluate fresh (so there is always something
   to compute logits from).
2. If that common-prefix length is at least `--cache-reuse` (default `1`,
   `0` disables reuse entirely), the cache is trimmed to the prefix
   (`llama_memory_seq_rm`) and only the remaining, differing suffix of the
   prompt is decoded.
3. Otherwise, the KV cache is cleared entirely (`llama_memory_clear`) and the
   whole prompt is decoded from scratch.

This is a plain longest-common-prefix match against the single most recently
decoded prompt — not a cache of multiple past prompts, and not a
similarity/fuzzy match. It exists to make repeated calls that share a long,
unchanged prefix (e.g. a growing chat transcript, or the same system prompt
across many requests) cheaper, at the cost of the linear scan and the memory
of holding one previous token sequence.

`jx_gen_result` reports `n_cached` (tokens reused) and `n_prompt` (total
prompt tokens) so callers can see how much was actually reused; the
`/v1/chat/completions` and `/v1/completions` responses surface these via the
`timings` object (`cache_n`, `prompt_n` = `n_prompt - n_cached`).

`embed()` always clears the KV cache first — prefix reuse does not apply to
embedding requests.

## Chat templating, grammar, and sampling

All three flow through llama.cpp's vendored `common` library rather than
being reimplemented in `jx-engine`:

- **Chat templates.** `jx_engine::load` calls `common_chat_templates_init`
  with the model plus an optional override (`--chat-template` or the
  contents of `--chat-template-file`), producing a
  `common_chat_templates_ptr`. `jinja` is always used (`common_chat_templates_apply`
  with `inputs.use_jinja = true`) — the `--jinja` flag exists only for
  command-line compatibility and is not itself a toggle in this codebase.
- **Applying a template.** `server.cpp`'s `parse_chat_inputs` builds a
  `common_chat_templates_inputs` from the request body (`messages`, `tools`,
  `tool_choice`, `parallel_tool_calls`, `grammar`, `json_schema`,
  `response_format`, `chat_template_kwargs`), and
  `common_chat_templates_apply(engine.chat_templates(), inputs)` renders it
  to a `common_chat_params` (`cp`), which carries the final prompt text
  (`cp.prompt`) plus any grammar the template itself wants applied for tool
  calling.
- **Grammar / structured output.** `apply_grammar()` in `server.cpp` takes
  whatever `common_chat_params` produced (from `tools`/`tool_choice`, from a
  caller-supplied `grammar` string, or from `json_schema`/`response_format`)
  and copies it — including grammar triggers and preserved tokens — into the
  `common_params_sampling` that will be handed to the sampler. `grammar_type`
  is chosen per request: `COMMON_GRAMMAR_TYPE_USER` when the caller passed
  `grammar` directly, `COMMON_GRAMMAR_TYPE_OUTPUT_FORMAT` when
  `json_schema`/`response_format` was used, otherwise
  `COMMON_GRAMMAR_TYPE_TOOL_CALLS` (letting the template's own tool-call
  grammar apply, if any). `/v1/completions` builds its grammar more directly:
  a caller-supplied `grammar` string is used as-is, or a `json_schema` is
  converted with `json_schema_to_grammar()`.
- **Sampling.** `parse_sampling()` reads `temperature`, `top_p`, `top_k`,
  `min_p`, `seed`, `repeat_penalty`, `repeat_last_n`, `presence_penalty`, and
  `frequency_penalty` from the request body into a `common_params_sampling`.
  `jx_engine::generate()` constructs a `common_sampler` from that struct via
  `common_sampler_init` and drives it with `common_sampler_sample`/
  `common_sampler_accept` — the sampling algorithms themselves (top-k/top-p/
  min-p/repetition penalties/grammar-constrained sampling) are llama.cpp's,
  not reimplemented here.
- **Parsing generated text back into a structured message.** For chat
  completions, `common_chat_parse()` turns the raw generated text back into a
  `common_chat_msg` (content, tool calls, reasoning content per
  `COMMON_REASONING_FORMAT_AUTO`), using the parser the template selected
  (`cp.parser`, loaded into `common_chat_parser_params`). Streaming diffs
  this against the previous partial parse (`common_chat_msg_diff::compute_diffs`)
  to emit only the incremental delta each step.

## Build layering

```
jx-engine (src/*.cpp)
    │  target_link_libraries: llama-common, llama, Threads::Threads
    ▼
llama-common          (<llama src>/common — chat templating, sampling,
    │                   JSON, grammar-to-schema, tokenization helpers)
    ▼
llama                 (<llama src> — the core inference library:
    │                   model loading, context, batched decode)
    ▼
ggml                  (<llama src>'s tensor/compute library; CPU by
                        default, CUDA/Vulkan/Metal/HIP/BLAS backends
                        selected via GGML_* CMake cache variables that
                        JX_ENGINE_CUDA/VULKAN/METAL/HIP/BLAS forward)
```

`<llama src>` is the llama.cpp source tree `CMakeLists.txt` resolves at
configure time, in this order:

1. `-DJX_ENGINE_LLAMA_DIR=<path>` — explicit override
2. `node_modules/@jx-holdings/llama-cpp-source` — the npm package installed
   by `npm ci` (canonical path; see [`building.md`](building.md))
3. `vendor/llama.cpp` — a manually placed source tree (gitignored, fallback
   only)

The build's only external input is the npm package: unlike the git submodule
this replaced, nothing under `vendor/` is tracked by this repository or
fetched automatically — it exists only as a manual escape hatch.
`CMakeLists.txt` builds only the llama.cpp libraries it needs:
`LLAMA_BUILD_TESTS`, `LLAMA_BUILD_EXAMPLES`, `LLAMA_BUILD_TOOLS`,
`LLAMA_BUILD_SERVER`, and `LLAMA_BUILD_APP` are all forced `OFF` (so
upstream's own `llama-server`, its app binary, and example binaries are
never built), `LLAMA_BUILD_COMMON` is forced `ON` (so the `common` library
`jx-engine` depends on is available), and `LLAMA_CURL` is forced `OFF`.
`jx-engine`'s include paths pull directly from `<llama src>/common`,
`<llama src>/vendor`, and `<llama src>/vendor/cpp-httplib` — `cpp-httplib`
(the HTTP server used in `server.cpp`) is llama.cpp's own vendored copy, not
a separate dependency of this repository.
