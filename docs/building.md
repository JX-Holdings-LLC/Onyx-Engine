# Building

## Prerequisites

- CMake ≥ 3.16
- Node.js/npm, to fetch the vendored llama.cpp source tree (see below)
- A C++17 compiler (the project sets `CMAKE_CXX_STANDARD 17`,
  `CMAKE_CXX_STANDARD_REQUIRED ON`)
- A C compiler (llama.cpp/ggml build C sources too — `CMakeLists.txt`
  declares `LANGUAGES C CXX`)
- Ninja (optional) — any CMake generator works; Ninja is just faster for
  incremental builds
- Threads (`find_package(Threads REQUIRED)`), which is part of the standard
  toolchain on all supported platforms

## Fetching the llama.cpp source (`npm ci`)

The build's only external input is llama.cpp's C/C++ source tree, vendored
as the npm package `@jxburros/llama-cpp-source` (a pinned, pruned copy of
llama.cpp — C/C++ sources, not a Node.js library) and declared as a
dependency in `package.json`:

```bash
npm ci   # or: npm install
```

This installs the source tree into `node_modules/@jxburros/llama-cpp-source`.
`CMakeLists.txt` resolves the llama.cpp source tree in this order:

1. `-DONYX_ENGINE_LLAMA_DIR=<path>` — explicit override, if passed
2. `node_modules/@jxburros/llama-cpp-source` — canonical, from `npm ci`
3. `vendor/llama.cpp` — a manually placed source tree (gitignored; fallback
   only, e.g. for offline work without npm)

If none of the three is present, the configure step fails with a clear
message (`llama.cpp source tree not found - run: npm ci`), so this step
cannot be skipped silently.

> **`npm ci` doesn't work yet.** `@jxburros/llama-cpp-source` has not been
> published to the npm registry, so `npm ci`/`npm install` currently fails
> with a 404/not-found error. Until the maintainer publishes it (see
> ["Packaging & publishing the vendor source package"](#packaging--publishing-the-vendor-source-package)
> below), install it locally from a tarball instead:
>
> ```bash
> packaging/make-vendor-package.sh
> npm install ./packaging/dist/jxburros-llama-cpp-source-*.tgz --no-save
> ```
>
> This is exactly how the npm-based build was verified during development.

## CPU build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target onyx-engine -j
```

(Equivalently: `npm run build`, which runs both `cmake` steps.)

This produces `build/onyx-engine`. No acceleration option is required — CPU
execution is the default in both `onyx-engine` (llama.cpp's CPU/ggml backend
is always built in) and at runtime (`-ngl 0`, or simply not offloading
layers).

`CMAKE_BUILD_TYPE` defaults to `Release` if not set.

## Acceleration option matrix

`onyx-engine`'s own CMake options forward to the corresponding `GGML_*` cache
variables that llama.cpp's build reads (`CMakeLists.txt` uses
`CACHE BOOL "" FORCE`, so these always take effect regardless of a prior
cached value):

| `onyx-engine` option | Default | Forces (in llama.cpp/ggml) |
|---|---|---|
| `ONYX_ENGINE_CUDA` | `OFF` | `GGML_CUDA` |
| `ONYX_ENGINE_VULKAN` | `OFF` | `GGML_VULKAN` |
| `ONYX_ENGINE_METAL` | `OFF` | `GGML_METAL` |
| `ONYX_ENGINE_HIP` | `OFF` | `GGML_HIP` |
| `ONYX_ENGINE_BLAS` | `OFF` | `GGML_BLAS` |

Enable exactly the one matching your hardware:

```bash
# CUDA (NVIDIA)
cmake -B build -DONYX_ENGINE_CUDA=ON

# Vulkan (cross-vendor GPU)
cmake -B build -DONYX_ENGINE_VULKAN=ON

# Metal (Apple Silicon / macOS)
cmake -B build -DONYX_ENGINE_METAL=ON

# ROCm / HIP (AMD)
cmake -B build -DONYX_ENGINE_HIP=ON

# BLAS
cmake -B build -DONYX_ENGINE_BLAS=ON

cmake --build build
```

Each option requires that backend's own SDK/toolchain to be installed and
discoverable (the CUDA toolkit for `ONYX_ENGINE_CUDA`, the Vulkan SDK for
`ONYX_ENGINE_VULKAN`, ROCm/HIP for `ONYX_ENGINE_HIP`, and so on) — `onyx-engine`'s
`CMakeLists.txt` only sets the flag that tells llama.cpp/ggml's own build
logic to look for it; it does not vendor or install those SDKs itself.
CPU execution remains available as a fallback in every build regardless of
which acceleration option (if any) is enabled — `-ngl 0` (or a GPU
offload that doesn't fully fit) runs the remaining layers on CPU.

Only one acceleration option is meant to be enabled per build; the code
comment in `CMakeLists.txt` notes this explicitly ("Exactly one of these is
typically enabled per build").

## What llama.cpp's own build is told to skip

To keep the build to just what `onyx-engine` links against,
`CMakeLists.txt` forces these llama.cpp cache variables:

| Variable | Value | Effect |
|---|---|---|
| `LLAMA_BUILD_TESTS` | `OFF` | no llama.cpp test binaries |
| `LLAMA_BUILD_EXAMPLES` | `OFF` | no llama.cpp example binaries |
| `LLAMA_BUILD_TOOLS` | `OFF` | no llama.cpp CLI tools |
| `LLAMA_BUILD_SERVER` | `OFF` | upstream's own `llama-server` is not built |
| `LLAMA_BUILD_APP` | `OFF` | no llama.cpp app binary |
| `LLAMA_BUILD_COMMON` | `ON` | the `common` library `onyx-engine` depends on is built |
| `LLAMA_CURL` | `OFF` | no libcurl dependency pulled in |

## Install target

```bash
cmake --install build --prefix /usr/local
```

Installs the `onyx-engine` binary to `<prefix>/bin` (the only `install()`
rule in `CMakeLists.txt` is `RUNTIME DESTINATION bin`). There is no
`install` rule for headers, libraries, or config files — `onyx-engine` is
shipped as a single self-contained binary.

## Test scripts

Both test scripts run fully offline: every model they need is generated
locally by a small numpy-only script, so nothing is downloaded from a model
hub and CI needs no external model access.

| Script | Exercises | Test model generator |
|---|---|---|
| `scripts/smoke-test.sh` | CLI `--help`/`--version`, all HTTP endpoints, streaming, parallel slots, context shift, `--mmproj` multimodal chat, logprobs, `--reasoning-budget` | `scripts/make-tiny-model.py` (tiny GGUF, generation + embedding instances), `scripts/make-tiny-mmproj.py` (tiny llava-style projector GGUF) |
| `scripts/safetensors-test.sh` | `scripts/convert-safetensors.py` directly, then `onyx_resolve_model()`'s conversion cache end-to-end through a running `onyx-engine` | `scripts/make-tiny-hf-model.py` (tiny HF directory: `config.json` + sharded-or-not `*.safetensors` + a byte-level BPE `tokenizer.json`) |

`scripts/safetensors-test.sh` is standalone (not called from
`smoke-test.sh` or `npm test`) since it needs `numpy` and exercises a
separate code path; it SKIPs (exit 0) rather than failing if `numpy` cannot
be installed, since CI always has network access and runs it for real.

## CI

`.github/workflows/ci.yml` runs on every push to `main`, every pull request,
and on demand (`workflow_dispatch`), on both `ubuntu-latest` and
`macos-latest`: it reads the pinned commit straight out of
`packaging/make-vendor-package.sh`, stages (and caches, keyed on that pin)
the vendor package, `npm install`s it, builds with `ccache`, then runs
`scripts/smoke-test.sh` and `scripts/safetensors-test.sh`. There is no
separate model-download step — the test scripts generate everything they
need themselves.

## Version string

`PROJECT_VERSION` (from `project(onyx-engine VERSION 0.1.0 ...)`) is baked
into the binary as the `ONYX_ENGINE_VERSION` preprocessor define, which is
what `onyx-engine --version` and the `Server:` HTTP response header report.

## Packaging & publishing the vendor source package

This section is for maintainers upgrading or publishing the vendored
llama.cpp source, not for people building `onyx-engine`. Consumers just run
`npm ci`.

`packaging/make-vendor-package.sh` builds `@jxburros/llama-cpp-source`
from a pinned llama.cpp commit:

```bash
packaging/make-vendor-package.sh [path-to-llama.cpp-checkout]
```

- **Pin.** The commit, package version, and llama.cpp's own CMake project
  version are hardcoded at the top of the script (currently commit
  `9723942adc518b43c4b95dc4dce6906903eb5e09`, tag `b10711`, package version
  `0.3.0-b10711.g9723942ad`).
- **Source.** With an argument, it stages from that local llama.cpp
  checkout. Without one, it downloads the pinned commit's tarball from
  GitHub into `packaging/dist/` — this is the only point in the whole
  workflow that needs network access, and it's only needed for packaging,
  never for building `onyx-engine` itself.
- **Pruning.** It stages a copy of the source tree with `docs/`, `tests/`,
  `examples/`, `benches/`, `media/`, `pocs/`, `ci/`, `app/`, `conversion/`,
  `requirements/`/`requirements.txt`, and `.git*`/`.devops` removed, plus all
  of `models/` except `ggml-vocab-llama-spm.gguf` (the vocab file
  `scripts/make-tiny-model.py` reads to build its smoke-test model), plus
  everything under `tools/` except `tools/mtmd` — `onyx-engine` builds exactly
  one thing out of `tools/` (the `mtmd` library, via `LLAMA_BUILD_MTMD=ON`,
  which `add_subdirectory()`s `tools/mtmd` directly), so `tools/server`,
  `tools/cli`, `tools/quantize`, and the rest of upstream's CLI tools are
  dropped. `tools/mtmd`'s own `CMakeLists.txt` links `vendor::hash`,
  `vendor::miniaudio`, `vendor::stb`, and `vendor::sheredom`, all of which
  live under `vendor/` and are kept. This pruning shrinks the published
  tarball from roughly 9.85 MB to 7.28 MB.
- **`.gitignore` stripping.** `npm pack` honors any `.gitignore` files it
  finds unless a `.npmignore` is present, and llama.cpp's own `.gitignore`
  would silently drop files the package needs (e.g. `*.gguf`). The script
  deletes every `.gitignore` in the staged tree and writes a minimal
  `.npmignore` instead, so nothing is dropped that the pruning step
  intentionally kept.
- **Package metadata.** It writes the staged tree's `package.json`
  (name, version, description, license, `homepage`, `repository`, plus
  `llamaCppCommit`/`llamaCppUpstream` fields recording provenance), then
  runs `npm pack` to produce `packaging/dist/jxburros-llama-cpp-source-<version>.tgz`.

To publish, once the tarball is built:

```bash
npm publish packaging/dist/jxburros-llama-cpp-source-*.tgz --access public
```

This needs npm credentials with publish rights on the `@jxburros` scope.
If that scope isn't available to you, rename it — it appears in exactly two
places: the `PKG_NAME` variable in `packaging/make-vendor-package.sh`, and
the `@jxburros/llama-cpp-source` dependency name in `package.json`.

**Until it's published**, `npm ci`/`npm install` fails on a clean checkout;
use `npm install ./packaging/dist/<tarball>.tgz --no-save` as a local
workaround after running the packaging script (see the note under
["Fetching the llama.cpp source"](#fetching-the-llama.cpp-source-npm-ci)
above).

**After the first publish**, run `npm install` once in a clean checkout and
commit the `package-lock.json` it generates, so that `npm ci` gives
reproducible, lockfile-pinned installs going forward. No `package-lock.json`
is committed yet.
