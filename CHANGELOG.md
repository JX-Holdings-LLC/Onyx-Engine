# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Changed

- Replaced the `vendor/llama.cpp` git submodule with the npm package
  `@jx-holdings/llama-cpp-source` (a pinned, pruned llama.cpp source tree —
  C/C++ sources, not a Node.js library) as the build's only external input,
  declared as a dependency in the new `package.json` and installed to
  `node_modules` by `npm ci`/`npm install`. `.gitmodules` is removed.
- `CMakeLists.txt` now resolves the llama.cpp source tree in order:
  `-DJX_ENGINE_LLAMA_DIR=<path>` override, then
  `node_modules/@jx-holdings/llama-cpp-source`, then a manually placed
  `vendor/llama.cpp` tree (gitignored, fallback only). `scripts/make-tiny-model.py`
  resolves the source tree with the same order (minus the CMake override).
- `CMakeLists.txt` additionally forces `LLAMA_BUILD_APP=OFF`.
- Quick start is now `npm ci` (or `npm install`), then
  `cmake -B build -DCMAKE_BUILD_TYPE=Release` and
  `cmake --build build --target jx-engine -j`. `package.json` adds npm
  scripts `build` (wraps those two `cmake` steps) and `test` (runs
  `scripts/smoke-test.sh`).

### Added

- `packaging/make-vendor-package.sh`: the maintainer-only script that stages
  the pinned llama.cpp commit (`9723942adc51ec2f2b7c9dcc86842934c479b336`,
  package version `0.3.0-b10711.g9723942ad`), prunes
  docs/tests/examples/benches/media/pocs/ci/app/conversion/requirements and
  all of `models/` except `ggml-vocab-llama-spm.gguf`, strips `.gitignore`
  files (`npm pack` would otherwise honor them and drop needed files),
  writes the package's `package.json`, and runs `npm pack` to produce the
  publishable tarball. Without a source argument it downloads the pinned
  commit's tarball from GitHub — network access is needed for packaging
  only, never for building `jx-engine`.

- Initial implementation of `jx-engine`: a single-model, OpenAI-compatible
  model-serving binary built on a vendored, pinned llama.cpp source tree
  (`@jx-holdings/llama-cpp-source`, an npm package — not yet published; see
  "Changed" above).
- CLI argument parsing (`src/args.h`, `src/args.cpp`) covering model path,
  alias, chat template overrides, network binding, API-key auth, context /
  batch / thread / GPU-offload sizing, flash attention, KV-cache prefix
  reuse, embedding mode, and pooling type.
- `jx_engine` (`src/engine.h`, `src/engine.cpp`): model/context loading,
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
  CUDA/Vulkan/Metal/ROCm(HIP)/BLAS acceleration via `JX_ENGINE_CUDA`,
  `JX_ENGINE_VULKAN`, `JX_ENGINE_METAL`, `JX_ENGINE_HIP`, `JX_ENGINE_BLAS`.
- Documentation: `README.md`, `docs/architecture.md`, `docs/api.md`,
  `docs/building.md`, `docs/jx-runtime-integration.md`.

Agent: Claude Code (Claude)
