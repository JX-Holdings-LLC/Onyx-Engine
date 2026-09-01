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
| `src/convert.h` / `src/convert.cpp` | `jx_resolve_model()`: GGUF passthrough vs. safetensors detection/conversion/caching (see [Safetensors resolution and conversion](#safetensors-resolution-and-conversion)) |
| `src/engine.h` / `src/engine.cpp` | `jx_engine`: owns the llama.cpp model + context (+ optional mtmd context), runs the batching loop, generation and embedding, tokenize/detokenize helpers |
| `src/server.h` / `src/server.cpp` | HTTP layer built on `cpp-httplib`: request parsing, response/SSE-frame construction, all route handlers |
| `src/main.cpp` | Entry point: parse args, resolve the model (`jx_resolve_model`), init the llama.cpp backend, load the model, run the server, tear down |

`server.cpp` depends on `engine.h` (to call generation/embedding/tokenize)
and on llama.cpp's `common` library headers (`chat.h`, `common.h`,
`json.h`, `json-schema-to-grammar.h`, `sampling.h`, `log.h`) for chat
templating, JSON, grammar construction, and sampling parameter types.
`engine.cpp` depends directly on `llama.h` (the core llama.cpp C API) plus
`common.h`/`sampling.h`/`log.h`.

## Concurrency model

- **One engine thread, `--parallel` request slots, continuous batching.**
  `jx_engine::load` creates the context with `n_seq_max = n_parallel` and
  allocates that many `jx_slot`s; slot `i` owns llama.cpp sequence id `i`.
  A dedicated thread (`jx_engine::loop`) ticks: it admits queued requests
  into idle slots, packs **one** `llama_batch` holding a prompt chunk for
  each slot still prefilling plus exactly one next-token row for each
  generating slot (each row tagged with that slot's sequence id, and the
  slot's logits row index recorded in `jx_slot::i_batch`), issues a single
  `llama_decode` for it (split into `n_batch`-sized views, halving the view
  size and retrying on `llama_decode() == 1`, fatal at size 1), and then
  samples each slot from its own `common_sampler` at its own row. `kv_unified`
  stays at llama.cpp's default `false`, so `n_ctx` is divided evenly across
  the slots: the per-slot budget is `llama_n_ctx_seq()`
  (`jx_engine::n_ctx_slot()`), and that — not `n_ctx` — is what a prompt has
  to fit into.
- **HTTP layer is multi-threaded; requests queue FIFO.** `jx-engine` uses
  `cpp-httplib`'s (`httplib::Server`) built-in thread pool. `generate()` is
  still a blocking call, but it now submits a `jx_gen_request` to the engine
  queue and waits on it instead of holding a global lock, so up to
  `--parallel` requests generate concurrently. When every slot is busy the
  head of the queue waits and nothing behind it jumps ahead. `embed()` still
  takes `jx_engine::mutex_` and runs serialized: embedding mode forces
  `n_parallel = 1` and starts no engine thread.
- **Streaming is one SSE write callback, fed by a per-request queue.**
  Chat/text completion streaming uses `httplib::Response::set_chunked_content_provider`.
  The provider lambda is called once (`offset == 0`) and does all its work —
  including the whole blocking `engine.generate()` call — inside that one
  invocation. The engine thread only ever *pushes* visible text into the
  request's `pieces` deque; the HTTP thread blocked in `generate()` pops from
  it and invokes the token callback (`jx_token_cb`), which is what writes
  each SSE frame. So a slow or stalled client can only back up its own
  request, never the engine loop or the other slots.
- **Cancellation.** The token callback returns `bool`; returning `false`
  (e.g. because `sink.write()` failed, meaning the client disconnected) marks
  the request cancelled. The engine loop notices at the top of its next tick,
  finishes that request with `jx_gen_result::finish = JX_FINISH_CANCEL` and
  releases its slot. Other slots are unaffected.

## Context shift

With `--context-shift`, a generating slot that would otherwise stop at its
per-slot context limit instead drops the oldest half of its non-preserved
context and keeps going (`jx_engine::context_shift`, mirroring
`llama-server`):

1. `n_keep` comes from `--keep` (`-1` = the whole prompt), `+1` if the vocab
   adds BOS, capped to `n_ctx_slot - 4`.
2. `n_discard = (n_past - n_keep) / 2`, clamped to `[0, n_left - 1]`.
3. `llama_memory_seq_rm(mem, seq, n_keep, n_keep + n_discard)` then
   `llama_memory_seq_add(mem, seq, n_keep + n_discard, n_past, -n_discard)`
   drop that window and shift everything after it down, and the slot's
   `cache_tokens` mirror is compacted the same way.
4. `jx_gen_result::truncated` is set; generation continues.

Without `--context-shift` (the default) the request finishes with
`JX_FINISH_LENGTH` at the context limit, exactly as in v1. If the loaded
context cannot shift (`llama_memory_can_shift()` is false, e.g. sliding-window
attention), `load()` forces the flag off with a warning on stderr — as does
loading a `--mmproj` projector (see
[Multimodal (`--mmproj`)](#multimodal---mmproj) below).

## KV-cache prefix reuse

Each slot keeps a copy of the token sequence currently materialized in *its*
sequence's KV cache (`jx_slot::cache_tokens`). When the engine loop admits a
queued request (`jx_engine::admit_queued`):

1. It scans the idle slots and picks the one whose `cache_tokens` share the
   longest common prefix with the new prompt — capped so at least one token
   is always left to evaluate fresh (so there is always something to compute
   logits from). Ties, and the case where reuse is off, fall back to the
   least-recently-used idle slot.
2. If that common-prefix length is at least `--cache-reuse` (default `1`,
   `0` disables reuse entirely), that slot's cache is trimmed to the prefix
   (`llama_memory_seq_rm`) and only the remaining, differing suffix of the
   prompt is decoded.
3. Otherwise the slot's sequence is dropped entirely
   (`llama_memory_seq_rm(mem, seq, -1, -1)`) and the whole prompt is decoded
   from scratch.

This is a plain longest-common-prefix match against the most recently decoded
prompt *of each slot* — not a cache of multiple past prompts per slot, no
similarity/fuzzy matching, and no RAM tier. It exists to make repeated calls
that share a long, unchanged prefix (e.g. a growing chat transcript, or the
same system prompt across many requests) cheaper, at the cost of the linear
scan and the memory of holding one previous token sequence per slot.

`jx_gen_result` reports `n_cached` (tokens reused) and `n_prompt` (total
prompt tokens) so callers can see how much was actually reused; the
`/v1/chat/completions` and `/v1/completions` responses surface these via the
`timings` object (`cache_n`, `prompt_n` = `n_prompt - n_cached`).

`embed()` always clears the KV cache first — prefix reuse does not apply to
embedding requests. Loading a `--mmproj` projector force-disables
`--cache-reuse` process-wide (see
[Multimodal (`--mmproj`)](#multimodal---mmproj) below) — a media chunk's KV
positions cannot be prefix-matched token-by-token.

## Multimodal (`--mmproj`)

`jx_engine::load` initializes an `mtmd_context` (`mtmd_init_from_file`) only
when `--mmproj` is passed; `mctx_` stays null otherwise, and
`has_mmproj()`/`supports_vision()`/`supports_audio()` all read straight
through to it (`mtmd_support_vision`/`mtmd_support_audio`), which is what
`GET /props`'s `modalities` and the per-request capability checks in
`server.cpp` report. Because a media chunk's KV positions can be neither
prefix-matched token-by-token nor partially discarded, loading a projector
force-disables `--cache-reuse` and `--context-shift` process-wide, with a
warning on stderr, mirroring `llama-server`.

**Marker rewrite.** `server.cpp::extract_media()` walks a chat request's
`messages` before the template renders: each `image_url`/`input_audio`
content part is decoded (data: URI or bare base64 — `reject_media_url()`
throws on `http://`/`https://`/`file://` before any capability check runs)
into a `jx_media_buffer`, and the part itself is replaced in place by a
`media_marker` part carrying `engine.media_marker()`
(`mtmd_get_marker(mctx_)`) — so the rendered prompt text contains one marker
token sequence per decoded buffer, in order, and `gp.prompt_text` +
`gp.media` (not `gp.prompt_tokens`) are what get passed to `generate()`.

**Media prefill at admission.** `jx_engine::prefill_media()` runs when a
newly admitted slot's request carries `media`: it never reuses KV
(`n_past` starts at 0 — media prompts are never prefix-matched, see the KV
reuse section above), calls `mtmd_helper_bitmap_init_from_buf` per buffer
and `mtmd_helper_eval_chunk_single` per resulting chunk, which runs the
vision/audio encoder and issues its own `llama_decode` calls directly
against `ctx_`. `n_past` is tracked strictly from `mtmd_helper_eval_chunk_single`'s
out-param rather than computed locally, because M-RoPE advances position
differently than the flat per-token counting the rest of the engine loop
uses. This is the one part of request handling that runs outside the shared
per-tick `llama_batch`: it **serializes the engine loop** for the duration of
one slot's media prefill (other slots' KV and the request queue are
otherwise unaffected — this is a momentary stall, not a global lock).

## Safetensors resolution and conversion

`main.cpp` calls `jx_resolve_model(args, err)` (`src/convert.cpp`) before
`jx_engine::load()` ever runs, and overwrites `args.model_path` with
whatever it returns:

1. **GGUF passthrough.** A `.gguf`-extension file, or any regular file
   starting with the `GGUF` magic bytes, is returned unchanged — no
   conversion involved.
2. **Safetensors detection.** Otherwise `-m` must be a directory containing
   `config.json` plus at least one `*.safetensors` file, or a
   `.safetensors` file whose parent directory satisfies the same; anything
   else is a startup error.
3. **Cache lookup.** The converted file lives at
   `<--convert-dir or model dir>/jx-cache/<model dir name>-<path hash>-f16.gguf`
   (the 8-hex-digit hash of the absolute source path keeps two models that
   share a directory name from colliding under a shared `--convert-dir`). It is
   reused as-is if it exists and is newer than every `*.safetensors` file
   and `config.json` in the source directory (mtime comparison) — so editing
   the source model (or its config) invalidates the cache automatically.
4. **Conversion.** On a cache miss, `resolve_converter_script()` locates
   `scripts/convert-safetensors.py` — via `$JX_ENGINE_CONVERT_SCRIPT` first,
   then paths relative to the running binary (`../scripts/`,
   `../../scripts/`), then (debug builds only) `JX_ENGINE_SOURCE_DIR` — and
   runs `python3 <script> <model_dir> --outfile <tmp> --outtype f16` via
   `popen`, streaming its combined output to stderr and keeping the last 20
   lines for the error message on failure. A successful run is renamed
   (falling back to copy+remove across filesystems) into place at the cache
   path.
5. **The converter itself** (`scripts/convert-safetensors.py`) is
   deliberately narrow: only the standard Hugging Face `LlamaForCausalLM`
   layout (`architectures`/`model_type` checked against
   `SUPPORTED_ARCHITECTURES`), optionally sharded via
   `model.safetensors.index.json`, with a byte-level BPE `tokenizer.json`
   (a SentencePiece `tokenizer.model` or any other tokenizer type is
   refused). It depends on nothing but `python3` + `numpy` — no
   `torch`/`transformers`, and nothing imported from the vendored llama.cpp
   tree — mapping HF tensor names to GGUF llama-arch names and writing the
   GGUF container format directly. Any structural mismatch (missing tensor,
   dtype it doesn't handle, vocab-size mismatch between `config.json` and
   `tokenizer.json`) raises a `ConvertError` with a specific message rather
   than producing a malformed GGUF.

## Reasoning budget (`--reasoning-budget`)

Own-code state machine, not a llama.cpp sampler feature — `jx_slot::rb_state_t`
runs per slot, driven by `on_sampled()` after every token:

- **`RB_OFF`** — no budget in effect for this request (`reasoning_budget <
  0`, the default). `build_reasoning_budget()` in `server.cpp` resolves the
  effective budget (`--reasoning-budget`, overridden per-request by
  `reasoning_budget_tokens`) and the template's own thinking tags
  (`cp.thinking_start_tag`/`thinking_end_tags[0]`, falling back to
  `<think>`/`</think>`) once per request; a negative budget short-circuits
  to a default-constructed (disabled) `jx_reasoning_budget` and the slot
  never leaves `RB_OFF`.
- **`RB_IDLE`** — scanning newly generated tokens for the start tag via a
  rolling multi-token match (`rb_start_match`). A template whose rendered
  generation prompt already ends inside the thinking block (deepseek-style,
  detected textually by `build_reasoning_budget()` via `start_in_prompt`)
  skips straight to `RB_COUNTING` instead.
- **`RB_COUNTING`** — counting generated tokens (`rb_count`) while also
  rolling-matching the end tag (`rb_end_match`); reaching the end tag
  naturally moves to `RB_DONE`, reaching the budget first moves to
  `RB_FORCING`.
- **`RB_FORCING`** — instead of sampling, tokens are emitted one at a time
  from `jx_reasoning_budget::forced` (`rb_forced_idx`) — the configured
  `--reasoning-budget-message` (if set and budget > 0) followed by the
  closing tag; budget `0` forces the closing tag immediately with no
  message.
- **`RB_DONE`** — passthrough forever after, whether the block closed
  naturally or was forced.

The response text is not otherwise special-cased: `common_chat_parse()`
still derives whatever reasoning/content split the template's own parser
produces from the final text.

## Logprobs

Captured inline during sampling, not as a post-hoc pass: `sample_slot()`
calls `compute_token_probs()` (`src/engine.cpp`) whenever the request set
`want_logprobs`, reading `llama_get_logits_ith(ctx_, tok_idx)` — the raw
logits row for the token about to be sampled, from the same batch row
`jx_slot::i_batch` recorded during `build_batch()` — and running a single
numerically-stable full-vocab softmax over it. This is deliberately the
*raw* model distribution, computed before any sampler-chain transform
(grammar, top-k/top-p/min-p, penalties), so a grammar-constrained or
otherwise heavily-steered pick still reports its true (possibly very low)
logprob, matching OpenAI semantics. The sampled token's own entry plus the
top `n_probs` alternatives (`std::partial_sort`, clamped to `[0, 25]`) are
recorded together as one `jx_token_probs`, appended to `jx_gen_result::probs`
in generation order and released to the caller in lockstep with the same
stop-holdback byte-accounting rule used for the generated text itself
(`collect_delivered_probs()`) — a token trimmed off by a stop sequence never
gets an entry. `server.cpp` then renders that same `jx_token_probs` list
into either the nested OpenAI chat shape (`logprobs.content[]`) or the
legacy flat `/v1/completions` shape, independently per endpoint.

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
    │  target_link_libraries: llama-common, llama, mtmd, Threads::Threads
    ▼
llama-common ─┐        (<llama src>/common — chat templating, sampling,
    │         │         JSON, grammar-to-schema, tokenization helpers)
    │     mtmd│        (<llama src>/tools/mtmd — multimodal projector:
    │         │         image/audio encoders, marker handling; built via
    │         │         LLAMA_BUILD_MTMD=ON, add_subdirectory()'d directly
    │         │         without the rest of tools/, which stays unbuilt)
    ▼         ▼
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
2. `node_modules/@jxburros/llama-cpp-source` — the npm package installed
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
`LLAMA_BUILD_MTMD` is forced `ON` — upstream's escape hatch for building
`tools/mtmd` as a standalone library directly (`add_subdirectory()`)
without going through `tools/CMakeLists.txt` and the rest of the (still-off)
tools tree — giving `jx-engine` multimodal projector support without
building any of upstream's own CLI tools. `MTMD_VIDEO` is forced `OFF`:
mtmd's video path shells out to an `ffmpeg` binary at runtime, and
`jx-engine` takes no runtime dependency on external processes.
`jx-engine`'s include paths pull directly from `<llama src>/common`,
`<llama src>/tools/mtmd`, `<llama src>/vendor`, and
`<llama src>/vendor/cpp-httplib` — `cpp-httplib` (the HTTP server used in
`server.cpp`) is llama.cpp's own vendored copy, not a separate dependency of
this repository.
