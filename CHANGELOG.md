# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added

- **`npm run vendor` — one command for the fresh-clone build.**
  `scripts/vendor.sh` stages the pinned llama.cpp vendor tarball with
  `packaging/make-vendor-package.sh` and extracts it into
  `node_modules/@jxburros/llama-cpp-source`, which is exactly the layout
  `npm ci` would produce and the one `CMakeLists.txt` resolves. It replaces
  the manual two-step (`packaging/make-vendor-package.sh` then `npm install
  ./packaging/dist/*.tgz --no-save`) documented in `README.md` and
  `docs/building.md`, and takes an optional path to an existing llama.cpp
  checkout (`npm run vendor -- /path/to/llama.cpp`), in which case nothing is
  downloaded. Extraction uses `tar`, not `npm install <tarball>`, so bash and
  tar are the only requirements — the same reasoning the CI and release
  workflows already used for their inline copies of these steps. The pinned
  commit and package version are unchanged.
- **`packaging/make-vendor-package.sh` falls back to `git fetch` when the
  source archive is unreachable.** It still prefers
  `github.com/ggml-org/llama.cpp/archive/<commit>.tar.gz`; when that fails it
  does a depth-1 `git fetch` of the same commit instead. Some corporate
  proxies and CI/agent sandboxes allow `git clone` over HTTPS but return 403
  for codeload archive URLs, which made the documented fallback unusable
  there. The fallback lives in this script rather than in `scripts/vendor.sh`
  so the cache-miss path in `.github/workflows/{ci,release}.yml`, which calls
  the packaging script directly, gets it too. Same pin either way, so the
  staged tree is identical.
- **CI builds on Windows** (`.github/workflows/ci.yml`, new `windows` matrix
  leg on `windows-latest`): configure, build, and `onyx-engine --version`.
  The test scripts are POSIX shell and are deliberately not run there. This
  exists because the first `v0.3.0` release run failed at the Windows
  configure step and nothing in CI had ever configured on Windows to catch
  it before a tag.

- **`--no-webui` accepted as a compatibility no-op.** JX Runtime's llama.cpp
  adapter (`src/backends/llamacpp.js`) passes `--no-webui` on every launch,
  and `onyx-engine` previously rejected the flag as unknown. JX Runtime's
  `onyxengine` adapter strips the flag rather than relying on this, so
  accepting it only matters for a launch that reuses `llama-server`
  arguments by hand. Added to both the parser and the curated `--help` text
  together, per `src/args.h`'s "parser and `--help` move together" rule.
- **Release workflow** (`.github/workflows/release.yml`), triggered on `v*`
  tag push (or manual dispatch with a `tag` input): builds a statically
  linked (`-DBUILD_SHARED_LIBS=OFF`) Release binary for `linux-x64`,
  `darwin-arm64`, `darwin-x64`, and `win32-x64`, and publishes each as
  `onyx-engine-<tag>-<platformKey>.tar.gz` (`.zip` on Windows) — binary plus
  `LICENSE`, flat, no subdirectory — attached to a GitHub Release for the
  tag alongside a `checksums.txt` (`sha256sum`-style, two-space separated).
  `<platformKey>` is exactly `linux-x64`/`darwin-arm64`/`darwin-x64`/
  `win32-x64`, matching Node's `${process.platform}-${process.arch}`, since
  JX Runtime's managed downloader (`jx-runtime backend install --engine
  onyx`) builds the asset URL from that pair. **JX Runtime's downloader
  pins the `v0.3.0` tag and these exact asset names**, so the first release
  cut from this repo must be tagged `v0.3.0` with a workflow run that
  produces all four platform archives plus `checksums.txt`, or that
  downloader will fail to find its asset.

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

- **`GET /props` reported the undivided context as `n_ctx`, so JX Runtime
  recorded a window `--parallel` times too large.** `n_ctx` (top level) and
  `default_generation_settings.n_ctx` both now report the **per-slot**
  context (`onyx_engine::n_ctx_slot()`, from `llama_n_ctx_seq`) — the window
  a single request is actually measured against — and stay equal to each
  other so either read path yields the same number. This is llama-server
  parity: its `get_res_props()` publishes
  `default_generation_settings.n_ctx` as `meta.slot_n_ctx`, which is its own
  `n_ctx_slot()` (`tools/server/server-context.cpp:4579,4173` at the pinned
  commit), and it has no top-level `n_ctx` at all.

  `--parallel N` divides the context — llama.cpp gives each slot roughly
  `n_ctx / N` tokens — and JX Runtime's adapter launches with
  `-c contextLength * slots` and passes `--parallel` on every launch, then
  reads this field back as the model's context length. Reporting the
  undivided total therefore told it the window was `slots` times larger than
  the engine would accept, so its request preflight admitted prompts the
  engine then refused. Reproduced against a built binary at
  `-c 512 --parallel 2`: `/props` reported `512`, the server log said
  `2 slots x 256 ctx`, and a 355-token prompt failed with
  `prompt (355 tokens) does not fit in the context window (256 tokens)`.
  After the fix `/props` reports `256`. The undivided value is not lost — it
  is the new top-level `n_ctx_total` — and the single-slot default is
  unchanged, since the two coincide there. `scripts/smoke-test.sh` now
  asserts both: `512`/`512` on the one-slot instance and `256` per slot with
  `n_ctx_total` `512` on the existing `-np 2` instance.
- **Release workflow: both platform failures that made the `v0.3.0` release
  publish zero assets.** Release run
  [33906146774](https://github.com/JX-Holdings-LLC/Onyx-Engine/actions/runs/33906146774)
  built `linux-x64` and `darwin-arm64` successfully but failed on the other
  two, and since the `release` job declares `needs: build`, nothing was
  published — the `v0.3.0` GitHub Release exists with **no assets at all**,
  so `jx-runtime backend install --engine onyx` cannot work today on any
  platform.
  - `build (win32-x64)` failed at configure with `CMake Error at
    CMakeLists.txt:3 (project): Generator Visual Studio 17 2022 could not
    find any instance of Visual Studio.` — the `windows-latest` image no
    longer ships VS 2022. The generator is no longer named: the configure
    step passes only `-A x64` and lets CMake select the newest installed
    Visual Studio. A version-pinned generator must be updated on every
    runner-image roll; the default never does.
  - `build (darwin-x64)` sat queued on `macos-13` for 24 hours and was
    cancelled — GitHub has retired that runner label. Moved to
    `macos-15-intel`, GitHub's supported x86_64 macOS image.
  - The "Locate binary" step no longer assumes the Windows output layout
    (which is no longer fixed, since the generator is not): it probes both
    the multi-config `build/Release/` and single-config `build/` paths on
    every platform and fails with an explicit `::error::` if neither holds a
    binary, instead of emitting an empty path that surfaced as a confusing
    failure two steps later.
  - The `release` job stays all-or-nothing, and `.github/workflows/release.yml`
    now says why in a comment: JX Runtime derives each asset name from
    `${process.platform}-${process.arch}` against one pinned tag, so a
    partial release would install cleanly on the platforms that built and
    fail with a bare 404 on the others — a per-machine breakage, from a
    release GitHub reports as green.

  **These fixes are reasoned from the failed run's logs and validated only
  as YAML; no release run has exercised them.** The `win32-x64` leg has
  still never completed successfully. **Maintainer action: `v0.3.0` must be
  re-cut** once this lands on `main` — run the `release` workflow via
  `workflow_dispatch` with tag input `v0.3.0`. `softprops/action-gh-release@v2`
  creates or updates the release for the tag, so this attaches assets to the
  existing empty `v0.3.0` release rather than erroring. The asset names
  (`onyx-engine-<tag>-<platformKey>.tar.gz`/`.zip`) and the `sha256sum`-style
  two-space `checksums.txt` format are unchanged; JX Runtime's
  `src/binaries.js` depends on both.
- **`--flash-attn`'s `--help` line now advertises `on|off|auto` rather than
  `on, off, auto`.** JX Runtime's generic llama.cpp adapter decides between
  passing `-fa <value>` and a bare `-fa` by matching `/on\|off\|auto|'on'/`
  against this line; the comma form matched neither alternative, so the probe
  classified `onyx-engine` as the old bare-toggle build — and a bare `-fa`
  exits with `error: -fa requires a value`. Verified by running that exact
  regex against a built binary's `--help`: `toggle` before, `value` after.
  Nothing was broken in practice, because `src/backends/onyxengine.js`
  overrides the probe (`async flashAttnStyle() { return 'value'; }`) instead
  of spawning `--help`; the mismatch mattered only for the parent `llamacpp`
  adapter pointed at an `onyx-engine` binary. Parser behavior is unchanged —
  the accepted values were and remain `on`/`off`/`auto`.
- **`docs/jx-runtime-integration.md` claimed the flash-attn probe already
  matched `onyx-engine`'s help text.** It did not, as above. The paragraph
  now states what the probe actually returned, what changed, and why the
  live adapter was unaffected.
- **`third_party/cpp-httplib/README.md` still used the pre-rename
  `jx-engine`/`jx-httplib` names** in its Onyx-authored provenance notes;
  the target is `onyx-httplib` and the binary is `onyx-engine`. Upstream
  cpp-httplib's own license text is untouched. (The note's claim that `nm -u
  build/onyx-engine | grep httplib` is empty was re-verified against a
  freshly built binary: 0 matches.)

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
