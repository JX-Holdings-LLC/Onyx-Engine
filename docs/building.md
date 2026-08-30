# Building

## Prerequisites

- CMake ≥ 3.16
- A C++17 compiler (the project sets `CMAKE_CXX_STANDARD 17`,
  `CMAKE_CXX_STANDARD_REQUIRED ON`)
- A C compiler (llama.cpp/ggml build C sources too — `CMakeLists.txt`
  declares `LANGUAGES C CXX`)
- Git, with submodule support, to fetch `vendor/llama.cpp`
- Ninja (optional) — any CMake generator works; Ninja is just faster for
  incremental builds
- Threads (`find_package(Threads REQUIRED)`), which is part of the standard
  toolchain on all supported platforms

## Submodule init

`vendor/llama.cpp` is a pinned git submodule. `CMakeLists.txt` checks for
`vendor/llama.cpp/CMakeLists.txt` and fails the configure step with a clear
message if it is missing, so this step cannot be skipped silently:

```bash
git submodule update --init --recursive
```

## CPU build

```bash
cmake -B build
cmake --build build
```

This produces `build/jx-engine`. No acceleration option is required — CPU
execution is the default in both `jx-engine` (llama.cpp's CPU/ggml backend
is always built in) and at runtime (`-ngl 0`, or simply not offloading
layers).

`CMAKE_BUILD_TYPE` defaults to `Release` if not set.

## Acceleration option matrix

`jx-engine`'s own CMake options forward to the corresponding `GGML_*` cache
variables that llama.cpp's build reads (`CMakeLists.txt` uses
`CACHE BOOL "" FORCE`, so these always take effect regardless of a prior
cached value):

| `jx-engine` option | Default | Forces (in llama.cpp/ggml) |
|---|---|---|
| `JX_ENGINE_CUDA` | `OFF` | `GGML_CUDA` |
| `JX_ENGINE_VULKAN` | `OFF` | `GGML_VULKAN` |
| `JX_ENGINE_METAL` | `OFF` | `GGML_METAL` |
| `JX_ENGINE_HIP` | `OFF` | `GGML_HIP` |
| `JX_ENGINE_BLAS` | `OFF` | `GGML_BLAS` |

Enable exactly the one matching your hardware:

```bash
# CUDA (NVIDIA)
cmake -B build -DJX_ENGINE_CUDA=ON

# Vulkan (cross-vendor GPU)
cmake -B build -DJX_ENGINE_VULKAN=ON

# Metal (Apple Silicon / macOS)
cmake -B build -DJX_ENGINE_METAL=ON

# ROCm / HIP (AMD)
cmake -B build -DJX_ENGINE_HIP=ON

# BLAS
cmake -B build -DJX_ENGINE_BLAS=ON

cmake --build build
```

Each option requires that backend's own SDK/toolchain to be installed and
discoverable (the CUDA toolkit for `JX_ENGINE_CUDA`, the Vulkan SDK for
`JX_ENGINE_VULKAN`, ROCm/HIP for `JX_ENGINE_HIP`, and so on) — `jx-engine`'s
`CMakeLists.txt` only sets the flag that tells llama.cpp/ggml's own build
logic to look for it; it does not vendor or install those SDKs itself.
CPU execution remains available as a fallback in every build regardless of
which acceleration option (if any) is enabled — `-ngl 0` (or a GPU
offload that doesn't fully fit) runs the remaining layers on CPU.

Only one acceleration option is meant to be enabled per build; the code
comment in `CMakeLists.txt` notes this explicitly ("Exactly one of these is
typically enabled per build").

## What llama.cpp's own build is told to skip

To keep the build to just what `jx-engine` links against,
`CMakeLists.txt` forces these llama.cpp cache variables:

| Variable | Value | Effect |
|---|---|---|
| `LLAMA_BUILD_TESTS` | `OFF` | no llama.cpp test binaries |
| `LLAMA_BUILD_EXAMPLES` | `OFF` | no llama.cpp example binaries |
| `LLAMA_BUILD_TOOLS` | `OFF` | no llama.cpp CLI tools |
| `LLAMA_BUILD_SERVER` | `OFF` | upstream's own `llama-server` is not built |
| `LLAMA_BUILD_COMMON` | `ON` | the `common` library `jx-engine` depends on is built |
| `LLAMA_CURL` | `OFF` | no libcurl dependency pulled in |

## Install target

```bash
cmake --install build --prefix /usr/local
```

Installs the `jx-engine` binary to `<prefix>/bin` (the only `install()`
rule in `CMakeLists.txt` is `RUNTIME DESTINATION bin`). There is no
`install` rule for headers, libraries, or config files — `jx-engine` is
shipped as a single self-contained binary.

## Version string

`PROJECT_VERSION` (from `project(jx-engine VERSION 0.1.0 ...)`) is baked
into the binary as the `JX_ENGINE_VERSION` preprocessor define, which is
what `jx-engine --version` and the `Server:` HTTP response header report.
