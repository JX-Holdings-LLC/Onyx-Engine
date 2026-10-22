[Documentation](../README.md) › [Development](README.md) › Building

# Building

Builds, acceleration backends and the install target. Fetching the pinned llama.cpp source comes first; testing, CI and releases have their own pages: [testing and CI](testing-and-ci.md), [releasing](releasing.md), [vendoring](vendoring.md).

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

The install script fetches llama.cpp's C/C++ source tree at the exact pinned
Git commit. Git and network access are needed for a fresh install:

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

`npm ci` uses the committed lockfile and `scripts/install-vendor.js` to fetch
and verify that commit. It does not request the unpublished npm package.

### `npm run vendor` — the local fallback

`scripts/vendor.sh` stages the pinned vendor tarball with
`packaging/make-vendor-package.sh` and extracts it into
`node_modules/@jxburros/llama-cpp-source`, which is the exact layout `npm
ci` would produce and the one `CMakeLists.txt` looks for. After it, build
normally:

```bash
npm run vendor
npm run build
```

It takes an optional path to a llama.cpp checkout you already have, in which
case nothing is downloaded:

```bash
npm run vendor -- /path/to/llama.cpp
```

Two details worth knowing:

- **It requires Bash, Git, npm, and tar.** npm packs the staged source;
  tar extracts it into the CMake search path.
- **It fetches the exact Git commit.** The packaging script uses a sparse
  checkout of the pinned commit and verifies it through Git's object hashes.

## CPU build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target onyx-engine -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
```

(Equivalently: `npm run build`, which runs both `cmake` steps via
`scripts/build.js`, one job per CPU; set `ONYX_BUILD_JOBS=N` to change that.)
Always give `-j` a number: a bare `-j` means *unlimited* jobs to GNU make,
which starts every llama.cpp translation unit at once and exhausts the memory
of a typical 16 GB machine (`Killed signal terminated program cc1plus`).

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
| `LLAMA_OPENSSL` | `OFF` | no Homebrew OpenSSL dependency pulled in |

## Install target

```bash
cmake --install build --prefix /usr/local
```

Installs the `onyx-engine` binary to `<prefix>/bin` (the only `install()`
rule in `CMakeLists.txt` is `RUNTIME DESTINATION bin`). There is no
`install` rule for headers, libraries, or config files — `onyx-engine` is
shipped without separate llama.cpp libraries. Standard OS libraries are
still required.

## Version string

The version comes from `package.json` (kept in step with `release.json` by release validation) through `.release/version.cmake`, which hands CMake
`APP_RELEASE_VERSION`. It is baked into the binary as the `ONYX_ENGINE_VERSION` preprocessor define, which is what `onyx-engine --version` and the `Server:` HTTP response header report.
`--version` also prints the llama.cpp build the binary was compiled against.
