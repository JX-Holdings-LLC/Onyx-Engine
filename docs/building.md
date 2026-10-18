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
and on demand (`workflow_dispatch`), on `ubuntu-latest`, `macos-latest`, and
`windows-latest`: it reads the pinned commit straight out of
`packaging/vendor/llama.cpp.conf`, stages (and caches, keyed on that pin)
the vendor package, installs it, and builds. There is no separate
model-download step — the test scripts generate everything they need
themselves.

The `linux` and `macos` legs build with `ccache` and then run
`scripts/smoke-test.sh` and `scripts/safetensors-test.sh`. The `windows` leg
is configure + build + `onyx-engine --version` only: it exists so a
Windows/MSVC toolchain break is caught on a pull request rather than during
a release (the first `v0.3.0` release run failed at exactly this step and
published nothing — see ["Release binaries"](#release-binaries)). The test
scripts are POSIX shell and are not run there; porting them would add no
coverage of the failure this leg is here to catch.

## Release binaries

`.github/workflows/release.yml` builds prebuilt `onyx-engine` binaries and
publishes them to this repo's GitHub Releases whenever a `v*` tag is
pushed (or via manual `workflow_dispatch` with a `tag` input, to re-run a
release without pushing a new tag).

For each of four platforms — `linux-x64` (ubuntu-22.04, for older-glibc
portability), `darwin-arm64` (`macos-latest`, Apple Silicon), `darwin-x64`
(`macos-15-intel`, macOS on Intel), and `win32-x64` (`windows-latest`, MSVC,
generator chosen by CMake) — the job stages the same pinned vendored llama.cpp source as CI, configures with
`-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF` (so `llama`,
`llama-common`, `ggml`, and `mtmd` are all linked in statically and the
result has no separate `libllama`/`libggml` libraries), builds the
`onyx-engine` target, runs `onyx-engine --version` as a sanity check, then
packages the binary flat with `LICENSE`, `LICENSE-cpp-httplib`, and
`LICENSE-llama.cpp`, with no subdirectory, as:

- `onyx-engine-<tag>-linux-x64.tar.gz`
- `onyx-engine-<tag>-darwin-arm64.tar.gz`
- `onyx-engine-<tag>-darwin-x64.tar.gz`
- `onyx-engine-<tag>-win32-x64.zip`

where `<tag>` is the pushed git tag (e.g. `v0.3.0`) and each `<platformKey>`
segment (`linux-x64`, `darwin-arm64`, `darwin-x64`, `win32-x64`) matches
Node's `${process.platform}-${process.arch}` — this is what lets JX
Runtime's managed downloader (`jx-runtime backend install --engine onyx`)
compute the right asset name for the machine it's running on.

A final `release` job downloads every platform's archive, writes a
`checksums.txt` next to them (`sha256sum`-style lines: a lowercase hex
digest, two spaces, then the filename — one line per archive), and
creates or updates the GitHub Release for the tag with all four archives
plus `checksums.txt` attached, with `generate_release_notes: true`.

Release builds disable host-native CPU tuning, OpenMP, and optional OpenSSL.
The x64 assets require an AVX2-capable CPU; the arm64 asset uses a portable
arm64 baseline. Windows uses the static MSVC runtime. Standard operating
system C libraries are still required.

**Cutting a release:**

```bash
git tag v0.4.0
git push origin v0.4.0
```

pushing the tag is the only step; the workflow does the rest. **JX
Runtime's downloader should pin the `v0.4.0` tag and the exact asset names
above** for this release. Once published, the `v0.4.0` release should contain
all four archives and `checksums.txt`.

### The `release` job is all-or-nothing, on purpose

`release` declares `needs: build` with no `if: always()`, so a single failing
platform blocks publication entirely. That is deliberate. JX Runtime derives
each asset's name from `${process.platform}-${process.arch}` against one
pinned tag, so a partial release installs cleanly on the platforms that built
and fails with a bare 404 on the ones that did not — a per-machine breakage
that looks like a network problem, from a release GitHub reports as green.
Failing the whole release makes the gap visible to the maintainer instead.

`fail-fast: false` on the build matrix is complementary rather than
contradictory: every platform still reports its own result, so one run shows
all the breakage at once rather than only the first failure.

### The first v0.3.0 attempt, and what changed

The first `v0.3.0` release run
([run 33906146774](https://github.com/JX-Holdings-LLC/Onyx-Engine/actions/runs/33906146774))
failed and **published a `v0.3.0` GitHub Release with zero assets**, which is
why `jx-runtime backend install --engine onyx` cannot work today. `linux-x64`
and `darwin-arm64` built fine; the other two legs did not, and `needs: build`
correctly withheld the release:

- **`win32-x64` failed at configure** with `CMake Error at CMakeLists.txt:3
  (project): Generator Visual Studio 17 2022 could not find any instance of
  Visual Studio.` The `windows-latest` image had moved past VS 2022, and the
  workflow pinned that generator by name. **Fix:** drop `-G` entirely and
  pass only `-A x64`, letting CMake select the newest installed Visual
  Studio. A pinned generator has to be updated on every runner-image roll;
  the default never does. The "Locate binary" step now probes both the
  multi-config (`build/Release/`) and single-config (`build/`) layouts and
  fails with an explicit error if neither holds a binary, instead of silently
  emitting an empty path.
- **`darwin-x64` was cancelled after 24 h queued** on `macos-13`, a runner
  label GitHub has retired. **Fix:** `macos-15-intel`, GitHub's supported
  x86_64 macOS image.
- **CI now configures and builds on Windows too**
  (`.github/workflows/ci.yml`, the `windows` matrix leg), so this class of
  failure is caught on a pull request rather than during a release. That leg
  is configure + build + `--version` only; the test scripts are POSIX shell
  and are not run there.
- **The first Windows CI run then found that `src/convert.cpp` had never
  compiled under MSVC** (`<sys/wait.h>`, `popen`/`WIFEXITED`, `/proc/self/exe`,
  `python3`). The generator fix was correct — all of llama.cpp built — and
  the converter launcher is now ported (`_popen`/`_pclose`, cmd.exe
  quoting, `GetModuleFileNameW`, `python`), so the Windows leg is the
  proof that a `win32-x64` release asset can be built at all.

The `v0.4.0` release should ship all four platform archives and
`checksums.txt`.
The release workflow requires an existing version-matched tag when dispatched.

## Version string

`PROJECT_VERSION` (from `project(onyx-engine VERSION 0.4.0 ...)`) is baked
into the binary as the `ONYX_ENGINE_VERSION` preprocessor define, which is
what `onyx-engine --version` and the `Server:` HTTP response header report.

## Packaging & publishing the vendor source package

This section is for maintainers upgrading or publishing a vendored source
tree, not for people building `onyx-engine`. Consumers just run `npm ci`.

### Vendor specs

What gets vendored is described by a **spec**, one file per upstream in
`packaging/vendor/`:

| Spec              | Package                      | Used by                                   |
| ----------------- | ---------------------------- | ----------------------------------------- |
| `llama.cpp.conf`  | `@jxburros/llama-cpp-source` | onyx-engine                               |
| `ggml.conf`       | `@jxburros/ggml-source`      | the BackTrack audio apps (standalone ggml) |

Each spec is a list of `KEY="value"` lines: the upstream repository, the
full 40-hex commit, upstream's own version (and build number, for
llama.cpp), the npm package name, the directories a fresh fetch checks out,
the paths pruned from the staged tree, the paths copied back after pruning,
and the paths an installed tree must contain. Four readers parse it —
`packaging/make-vendor-package.sh` and `scripts/vendor.sh` (sourced as Bash
through `packaging/vendor/lib.sh`), `scripts/install-vendor.js`, and
`CMakeLists.txt` — so it stays one value per line, no expansions.

The scripts themselves carry nothing specific to llama.cpp, so another
repository reuses `packaging/make-vendor-package.sh`,
`packaging/vendor/lib.sh`, `scripts/vendor.sh` and `scripts/install-vendor.js`
unchanged, adding its own spec and listing it in its `package.json`'s
`"vendor"` array (`["llama.cpp"]` here, `["ggml"]` in BackTrack).

**The ggml pin follows the llama.cpp pin.** `ggml.conf` pins the ggml
revision the llama.cpp pin last synced (recorded upstream in llama.cpp's
`scripts/sync-ggml.last`), so a ggml fix or a new GPU backend reaches
onyx-engine and the audio apps together. After moving the llama.cpp pin,
run:

```bash
scripts/check-ggml-pin.sh                 # checks packaging/vendor/ggml.conf
scripts/check-ggml-pin.sh ../BackTrack/packaging/vendor/ggml.conf
```

It prints `ok`, or the ggml commit to move to and exits 1.

### Building a package

```bash
packaging/make-vendor-package.sh [--spec NAME|FILE] [path-to-upstream-checkout]
```

- **Spec.** `--spec` names a file in `packaging/vendor/` (`llama.cpp`, the
  default, or `ggml`) or gives a path to one.
- **Pin.** For llama.cpp: commit `9723942adc518b43c4b95dc4dce6906903eb5e09`,
  build `b10711`, package version `0.3.0-b10711.g9723942ad`. For ggml:
  commit `36da57138425487184aa1da2eee2cde155909c6f`, package version
  `0.22.0.g36da57138`. The version is always
  `<upstream version>[-b<build>].g<9-char commit>`.
- **Source.** With a path, it stages from that local checkout (which must
  be at the pinned commit). Without one, it does a depth-1 `git fetch` of
  the pinned commit, sparse to the spec's directories, into
  `packaging/dist/<spec>/` — the only step that needs network access.
- **Pruning.** For llama.cpp: `docs/`, `tests/`, `examples/`, `benches/`,
  `media/`, `pocs/`, `ci/`, `app/`, `conversion/`,
  `requirements/`/`requirements.txt` and `.github`/`.devops` are removed, and
  so are all of `models/` except `ggml-vocab-llama-spm.gguf` (the vocab file
  `scripts/make-tiny-model.py` reads) and everything under `tools/` except
  `tools/mtmd` — `onyx-engine` builds exactly one thing out of `tools/`, the
  `mtmd` library (via `LLAMA_BUILD_MTMD=ON`, which `add_subdirectory()`s
  `tools/mtmd` directly); its `CMakeLists.txt` links `vendor::hash`,
  `vendor::miniaudio`, `vendor::stb` and `vendor::sheredom`, all under
  `vendor/` and kept. For ggml: everything but `cmake/`, `include/`, `src/`
  and the top-level files.
- **`.gitignore` stripping.** `npm pack` honors any `.gitignore` files it
  finds unless a `.npmignore` is present, and upstream's `.gitignore` would
  silently drop files the package needs (e.g. `*.gguf`). The script deletes
  every `.gitignore` in the staged tree and writes a minimal `.npmignore`.
- **Package metadata.** The staged tree gets a `.vendor-source-commit`
  stamp and a `package.json` (name, version, description, license,
  `homepage`, `repository`, plus `vendorName`/`vendorCommit`/`vendorUpstream`
  recording provenance), then `npm pack` produces
  `packaging/dist/<spec>/<package>-<version>.tgz`.

`scripts/vendor.sh [--spec NAME] [path]` runs the same build and extracts the
tarball into `node_modules/<package>`; `--install-only` skips the build and
installs a tarball already in `packaging/dist/<spec>/` (CI restores it from
its cache).

### Publishing (`.github/workflows/publish-vendor.yml`)

Publishing is automated: `.github/workflows/publish-vendor.yml`,
`workflow_dispatch`-only with a `spec` choice (`llama.cpp` or `ggml`) and a
`dry_run` boolean input (default `true`), runs
`packaging/make-vendor-package.sh --spec <spec>` to build the tarball,
checks `npm view <package>@<version>` and skips with a notice if that exact
version is already on the registry (so re-dispatching after a successful
publish is a no-op, not an error), and otherwise runs

```bash
npm publish packaging/dist/<spec>/<package>-<version>.tgz --access public --provenance
```

(with `--dry-run` appended when `dry_run` is `true`).

**One-time setup**, before the first real run:

1. Create an npm **granular access token** with publish rights scoped to
   `@jxburros`, and store it as this repo's `NPM_TOKEN` secret (Settings →
   Secrets and variables → Actions) — this is what
   `actions/setup-node@v4`'s `registry-url` plus the workflow's
   `NODE_AUTH_TOKEN` env var authenticate with. **Or**, skip the token
   entirely and set up [npm Trusted
   Publishing](https://docs.npmjs.com/trusted-publishers) for this
   repository and the `publish-vendor.yml` workflow — npm then accepts the
   OIDC identity the workflow already requests (`permissions: id-token:
   write`, needed for `--provenance` regardless) instead of a stored
   secret.
2. If `@jxburros` isn't available to you, rename it: it is the `PKG_NAME`
   line of each spec in `packaging/vendor/`, and the
   `node_modules/@jxburros/llama-cpp-source` path `CMakeLists.txt` and the
   workflows look in.

**To run it:** Actions → `publish-vendor` → Run workflow. Run it once with
`dry_run` left `true` (checked into the input default) to exercise the
whole path — build, version check, `npm publish --dry-run` — without
actually publishing, then run it again with `dry_run` set to `false`.

**When to run it again:** only when a pin in `packaging/vendor/` changes (a
new commit means a new package version), not on every push — the
version-check step makes an accidental extra dispatch harmless either way.

Publishing this optional package is no longer required for a clean build.
`npm ci` fetches and verifies the pinned Git commit directly, and the
committed `package-lock.json` records the local install script.
