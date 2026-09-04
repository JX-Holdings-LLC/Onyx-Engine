# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Changed

- **Project renamed from JX Engine to Onyx Engine.** All identifiers,
  targets, macros, env vars, and docs moved from the `jx`/`JX` prefix to
  `onyx`/`ONYX` (binary `jx-engine` → `onyx-engine`, class `jx_engine` →
  `onyx_engine`, `JX_ENGINE_*` build options → `ONYX_ENGINE_*`, etc.). No
  behavior changed. JX Runtime, the separate Node.js project this engine is
  built to be spawned by, keeps its own name.
- **`cpp-httplib` is now onyx-engine's own vendored dependency**
  ([`third_party/cpp-httplib`](third_party/cpp-httplib), v0.54.1), compiled
  as the `onyx-httplib` target, instead of being reached for inside the
  vendored llama.cpp tree. `server.cpp` previously included
  `<llama src>/vendor/cpp-httplib/httplib.h` while the implementation came
  out of `libllama-common.so`, which links upstream's copy statically and
  re-exports ~1200 `httplib::*` symbols — so onyx-engine's HTTP layer rode on
  a private implementation detail of `llama-common`, and a llama.cpp bump
  could break it for reasons unrelated to inference. `onyx-engine`'s include
  paths into the llama.cpp tree are now just `common/` and `tools/mtmd/`.
  - cpp-httplib's tuning macros (`CPPHTTPLIB_TCP_NODELAY`,
    `CPPHTTPLIB_LISTEN_BACKLOG`, `CPPHTTPLIB_REQUEST_URI_MAX_LENGTH`,
    `CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH`) are `PUBLIC` on the
    local target, so the header and the implementation agree. Upstream sets
    them `PRIVATE`, which left `server.cpp` compiling `httplib::Server` with
    a different `tcp_nodelay_` initializer than `httplib.cpp` did — an ODR
    mismatch. It was latent, not live: the effective value came from the
    out-of-line constructor in `httplib.cpp`, so Nagle was disabled either
    way.
  - `onyx-httplib` is linked ahead of `llama-common` deliberately; in the other
    order 15 httplib symbols still resolved to llama.cpp's shared library.

### Fixed

- **`--version` and `/props`'s `build_info` reported onyx-engine's own git
  commit as the llama.cpp build info.** They used llama.cpp's
  `llama_build_info()`, whose value comes from `cmake/build-info.cmake`
  running `git rev-parse` in the llama.cpp source directory. In the canonical
  build that directory is `node_modules/@jxburros/llama-cpp-source`, which
  has no `.git`, so git walked up into onyx-engine's repository: a build at
  onyx-engine commit `c0394c2` reported `b20-c0394c2`. `CMakeLists.txt` now
  resolves the pin itself (`ONYX_ENGINE_LLAMA_PIN`) from the npm package's
  `package.json` version, falling back to git only when the repository's top
  level is the llama.cpp tree itself, and `unknown` otherwise. `build_info`
  now reads `onyx-engine/0.2.0 (llama.cpp b10711-9723942ad)`, matching the
  shape `docs/api.md` already documented.

## [0.2.0] - 2026-09-01

v2 implements all six former roadmap items: real parallel request slots with
continuous batching, context shift, logprobs, reasoning-budget control,
multimodal input via `--mmproj`, and safetensors model support.

### Added

- **Parallel request slots + continuous batching.** `onyx_engine` now runs a
  dedicated batching-loop thread over `--parallel`/`-np` slots sharing one
  `llama_context` (slot `i` = llama.cpp sequence id `i`, per-slot context
  budget from `llama_n_ctx_seq`): each tick packs one next-token row per
  generating slot plus prompt chunks for prefilling slots into a single
  shared `llama_batch`, decodes it (in `n_batch`-sized views, halving and
  retrying on `llama_decode() == 1`), and samples each slot from its own
  `common_sampler` at its own logits row. Requests beyond the slot count
  queue FIFO; a stalled/slow client only backs up its own request. The v1
  KV-prefix-reuse contract (longest-common-prefix match, `--cache-reuse`
  minimum, `timings.cache_n`) is preserved per slot.
- **Context shift** (`--context-shift`/`--no-context-shift`, `--keep N`): a
  generating slot that would otherwise stop at its per-slot context limit
  instead drops `(n_left)/2` tokens after the preserved `--keep` prefix
  (`llama_memory_seq_rm` + `llama_memory_seq_add`) and keeps generating.
  Off by default (preserves v1's stop-at-limit behavior); force-disabled
  with a warning if the context's memory layout cannot shift (e.g.
  sliding-window attention) or if `--mmproj` is loaded.
- **Logprobs** on both endpoints, computed in-engine from the raw model
  distribution (`llama_get_logits_ith` at the slot's sampled row, full-vocab
  softmax, top-N via `std::partial_sort`) — unaffected by
  grammar/sampler-chain constraints, matching OpenAI semantics.
  `/v1/chat/completions` accepts `logprobs`/`top_logprobs` and returns the
  modern nested `logprobs.content[]` shape (streaming frames carry logprobs
  independently of the parsed-message delta diffing); `/v1/completions`
  accepts the legacy `logprobs: N` form and returns the flat
  `tokens`/`token_logprobs`/`top_logprobs`/`text_offset` shape. Streamed
  entries are released in lockstep with the existing stop-holdback
  byte-accounting rule; a token trimmed off by a stop sequence gets no
  entry.
- **Reasoning-budget control** (`--reasoning-budget`,
  `--reasoning-budget-message`, per-request `reasoning_budget_tokens`): a
  per-slot state machine (`onyx_slot::rb_state_t`) in the engine loop, not a
  llama.cpp sampler feature — rolling-matches the chat template's own
  thinking tags (falling back to `<think>`/`</think>`), counts generated
  tokens once inside the block, and force-emits
  `--reasoning-budget-message` (if set) plus the closing tag once the
  budget is spent (budget `0`: closing tag only, immediately). Handles
  deepseek-style templates whose rendered generation prompt already opens
  the thinking block.
- **Multimodal input via `--mmproj`.** `onyx_engine` loads an `mtmd_context`
  (`mtmd_init_from_file`) when `--mmproj` is passed; `GET /props`'s
  `modalities` now reports real vision/audio support. Chat requests accept
  `image_url` (and `input_audio`, projector permitting) content parts as
  `data:` URIs or raw base64 only — `http://`/`https://`/`file://` are
  rejected with `400` by design, since `onyx-engine` performs no network or
  filesystem I/O on a request's behalf. Media parts are rewritten to mtmd
  marker text before the chat template renders, then prefilled through
  mtmd at slot admission (running the vision/audio encoder and its own
  `llama_decode` calls inline, tracking position from mtmd's out-param for
  M-RoPE correctness) — this momentarily serializes the batching loop for
  that one slot's prefill. Loading a projector force-disables
  `--cache-reuse` and `--context-shift` process-wide, with a startup
  warning: a media chunk's KV positions can be neither prefix-matched nor
  partially discarded. `CMakeLists.txt` adds `LLAMA_BUILD_MTMD=ON`
  (building `tools/mtmd` as a standalone library without the rest of the
  `tools/` tree) and forces `MTMD_VIDEO=OFF` (mtmd's video path shells out
  to `ffmpeg` at runtime; `onyx-engine` takes no such dependency).
  `scripts/make-tiny-mmproj.py` generates a tiny llava-style projector for
  offline testing.
- **Safetensors model support.** `-m` now also accepts a Hugging Face model
  directory (or a `.safetensors` file inside one); `src/convert.{h,cpp}`
  adds `onyx_resolve_model()`, called from `main.cpp` before `onyx_engine::load`:
  GGUF passthrough, safetensors detection, conversion via
  `scripts/convert-safetensors.py`, and mtime-based cache reuse under
  `<model dir>/onyx-cache/` (or `--convert-dir`). `--convert-dir DIR`
  configures the cache location. `scripts/convert-safetensors.py` converts
  the standard HF `LlamaForCausalLM` layout (optionally sharded via
  `model.safetensors.index.json`) with a byte-level BPE `tokenizer.json`,
  using only `python3` + `numpy` — no `torch`/`transformers`, nothing
  imported from the vendored llama.cpp tree; anything else (a different
  architecture, a SentencePiece `tokenizer.model`, an unhandled dtype) is
  refused with a specific error. `scripts/make-tiny-hf-model.py` generates
  a tiny HF model for tests; `scripts/safetensors-test.sh` is the
  standalone end-to-end test (generate → convert → serve, `numpy`-optional
  with a graceful skip).
- **CI** (`.github/workflows/ci.yml`): builds against the pinned vendor
  source and runs the full offline test suite (`scripts/smoke-test.sh`,
  `scripts/safetensors-test.sh`) on `ubuntu-latest` and `macos-latest`, on
  every push to `main`, every pull request, and on demand. The vendor
  package is staged once and cached by pin; all test models are generated
  locally, so CI downloads no models.
- Full v2 CLI flag surface defined up front in `src/args.cpp`/`--help`,
  matching the parser: `--mmproj`, `--convert-dir`, `--context-shift`/
  `--no-context-shift`, `--keep`, `--reasoning-budget`,
  `--reasoning-budget-message`; `--parallel`/`-np` now allocates real
  concurrent slots instead of being accepted-but-ignored.

### Fixed

- Corrected a corrupted `LLAMA_COMMIT` pin in
  `packaging/make-vendor-package.sh`: only the first 12 hex characters of
  the recorded commit matched a real llama.cpp commit. Repinned to the full
  commit for tag `b10711`
  (`9723942adc518b43c4b95dc4dce6906903eb5e09`), so the vendor package can
  actually be staged from upstream.

### Changed

- `packaging/make-vendor-package.sh` now also prunes everything under
  `tools/` except `tools/mtmd` (the one thing `onyx-engine` builds out of that
  tree, via `LLAMA_BUILD_MTMD=ON`) — upstream's own CLI tools
  (`tools/server`, `tools/cli`, `tools/quantize`, etc.) are never configured
  and were dead weight in the tarball; this shrinks it from roughly 9.85 MB
  to 7.28 MB. `tools/mtmd`'s `vendor::hash`/`vendor::miniaudio`/
  `vendor::stb`/`vendor::sheredom` link dependencies (all under `vendor/`)
  are kept.
- `CMakeLists.txt`'s `project()` version bumped `0.1.0` → `0.2.0`;
  `package.json`'s version bumped to match.

## [0.1.0] - 2026-09-01

### Changed

- Replaced the `vendor/llama.cpp` git submodule with the npm package
  `@jxburros/llama-cpp-source` (a pinned, pruned llama.cpp source tree —
  C/C++ sources, not a Node.js library) as the build's only external input,
  declared as a dependency in the new `package.json` and installed to
  `node_modules` by `npm ci`/`npm install`. `.gitmodules` is removed.
- `CMakeLists.txt` now resolves the llama.cpp source tree in order:
  `-DONYX_ENGINE_LLAMA_DIR=<path>` override, then
  `node_modules/@jxburros/llama-cpp-source`, then a manually placed
  `vendor/llama.cpp` tree (gitignored, fallback only). `scripts/make-tiny-model.py`
  resolves the source tree with the same order (minus the CMake override).
- `CMakeLists.txt` additionally forces `LLAMA_BUILD_APP=OFF`.
- Quick start is now `npm ci` (or `npm install`), then
  `cmake -B build -DCMAKE_BUILD_TYPE=Release` and
  `cmake --build build --target onyx-engine -j`. `package.json` adds npm
  scripts `build` (wraps those two `cmake` steps) and `test` (runs
  `scripts/smoke-test.sh`).

### Added

- `packaging/make-vendor-package.sh`: the maintainer-only script that stages
  the pinned llama.cpp commit (`9723942adc51ec2f2b7c9dcc86842934c479b336` as
  recorded at the time — the tail was corrupted; corrected in 0.2.0, see
  below — package version `0.3.0-b10711.g9723942ad`), prunes
  docs/tests/examples/benches/media/pocs/ci/app/conversion/requirements and
  all of `models/` except `ggml-vocab-llama-spm.gguf`, strips `.gitignore`
  files (`npm pack` would otherwise honor them and drop needed files),
  writes the package's `package.json`, and runs `npm pack` to produce the
  publishable tarball. Without a source argument it downloads the pinned
  commit's tarball from GitHub — network access is needed for packaging
  only, never for building `onyx-engine`.

- Initial implementation of `onyx-engine`: a single-model, OpenAI-compatible
  model-serving binary built on a vendored, pinned llama.cpp source tree
  (`@jxburros/llama-cpp-source`, an npm package — not yet published; see
  "Changed" above).
- CLI argument parsing (`src/args.h`, `src/args.cpp`) covering model path,
  alias, chat template overrides, network binding, API-key auth, context /
  batch / thread / GPU-offload sizing, flash attention, KV-cache prefix
  reuse, embedding mode, and pooling type.
- `onyx_engine` (`src/engine.h`, `src/engine.cpp`): model/context loading,
  text generation with stop-sequence handling and streaming token callback,
  pooled+L2-normalized embeddings, tokenize/detokenize helpers, and
  longest-common-prefix KV-cache reuse across requests (`--cache-reuse`).
- HTTP server (`src/server.h`, `src/server.cpp`) built on llama.cpp's
  vendored `cpp-httplib`, exposing `GET /health`, `GET /props`,
  `GET /v1/models`, `POST /tokenize`, `POST /detokenize`,
  `POST /apply-template`, `POST /v1/chat/completions`,
  `POST /v1/completions`, and `POST /v1/embeddings`, with SSE streaming
  (final usage+timings frame, `data: [DONE]` terminator), jinja chat
  templating with tool-call parsing and streamed tool-call deltas, and
  GBNF/`json_schema`/`response_format` structured output via llama.cpp's
  `common` library.
- CMake build (`CMakeLists.txt`) with CPU-only default and opt-in
  CUDA/Vulkan/Metal/ROCm(HIP)/BLAS acceleration via `ONYX_ENGINE_CUDA`,
  `ONYX_ENGINE_VULKAN`, `ONYX_ENGINE_METAL`, `ONYX_ENGINE_HIP`, `ONYX_ENGINE_BLAS`.
- Documentation: `README.md`, `docs/architecture.md`, `docs/api.md`,
  `docs/building.md`, `docs/jx-runtime-integration.md`.

Agent: Claude Code (Claude)
