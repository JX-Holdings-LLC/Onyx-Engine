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
> below), run the local fallback instead — one command, no npm registry:
>
> ```bash
> npm run vendor      # or, without npm:  bash scripts/vendor.sh
> ```

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

- **It extracts with `tar`, not `npm install <tarball>`**, so the only tools
  it needs are bash and tar. `.github/workflows/{ci,release}.yml` do the same
  two steps inline for the same reason (self-hosted runners need no npm), and
  the resulting tree is byte-identical either way.
- **It fetches the pinned commit over HTTPS, falling back to git.**
  `make-vendor-package.sh` first tries the GitHub source archive
  (`.../archive/<commit>.tar.gz`); if that is unreachable — some corporate
  proxies and CI/agent sandboxes allow `git clone` but return 403 for
  codeload archive URLs — it falls back to a depth-1 `git fetch` of the same
  commit. The fallback lives in `make-vendor-package.sh` rather than in
  `scripts/vendor.sh` so CI's cache-miss path, which calls the packaging
  script directly, gets it too. Either way the commit is the same pin, so
  the staged tree is identical.

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
and on demand (`workflow_dispatch`), on `ubuntu-latest`, `macos-latest`, and
`windows-latest`: it reads the pinned commit straight out of
`packaging/make-vendor-package.sh`, stages (and caches, keyed on that pin)
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
result is a single self-contained binary with no `libllama`/`libggml`
alongside it), builds the `onyx-engine` target, runs `onyx-engine --version`
as a sanity check, then packages the binary flat — `onyx-engine` (or
`onyx-engine.exe` on Windows) plus `LICENSE`, no subdirectory — as:

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

**Cutting a release:**

```bash
git tag v0.3.1
git push origin v0.3.1
```

pushing the tag is the only step; the workflow does the rest. **JX
Runtime's downloader pins the `v0.3.1` tag and the exact asset names above**
(JX Runtime is moving its pin to `v0.3.1` in parallel with this release) —
see the `[0.3.1]` entry in `CHANGELOG.md` — so the first release cut from
this repo must be tagged `v0.3.1` and must succeed in producing all four
archives plus `checksums.txt`, or JX Runtime's managed install will fail to
find its asset.

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

**These fixes are not yet proven by a real release run**, but the Windows
half is proven by CI: the `windows` leg of `ci.yml` builds the same static
MSVC configuration `release.yml` ships and runs `--version` on the result,
and it is green (the first Windows build of `onyx-engine.exe` ever). The
`macos-15-intel` leg is exercised only by a release run. **`v0.3.1` must be cut
by the maintainer** once this lands on `main`: run the `release` workflow
via `workflow_dispatch` with tag input `v0.3.1` from `main` (JX Runtime is
moving its pin to `v0.3.1` in parallel). `softprops/action-gh-release@v2`
creates or updates the release for a given tag, so this creates a fresh
`v0.3.1` release rather than touching the abandoned, asset-less `v0.3.0`
one.

## Version string

`PROJECT_VERSION` (from `project(onyx-engine VERSION 0.1.0 ...)`) is baked
into the binary as the `ONYX_ENGINE_VERSION` preprocessor define, which is
what `onyx-engine --version` and the `Server:` HTTP response header report.

## Packaging & publishing the vendor source package

This section is for maintainers upgrading or publishing the vendored
llama.cpp source, not for people building `onyx-engine`. Consumers just run
`npm ci` (or, today, `npm run vendor`).

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
  GitHub into `packaging/dist/`, and if that download is unavailable
  (proxies that block codeload archive URLs but allow git) it falls back to a
  depth-1 `git fetch` of the same commit into the same directory — this is
  the only point in the whole workflow that needs network access, and it's
  only needed for packaging, never for building `onyx-engine` itself.
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

### Publishing (`.github/workflows/publish-vendor.yml`)

Publishing is automated: `.github/workflows/publish-vendor.yml`,
`workflow_dispatch`-only with a `dry_run` boolean input (default `true`),
reads the pin the same way `ci.yml` does, runs
`packaging/make-vendor-package.sh` to build the tarball, checks `npm view
@jxburros/llama-cpp-source@<version>` and skips with a notice if that exact
version is already on the registry (so re-dispatching after a successful
publish is a no-op, not an error), and otherwise runs

```bash
npm publish packaging/dist/jxburros-llama-cpp-source-*.tgz --access public --provenance
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
2. If `@jxburros` isn't available to you, rename it — it appears in exactly
   two places: the `PKG_NAME` variable in
   `packaging/make-vendor-package.sh`, and the `@jxburros/llama-cpp-source`
   dependency name in `package.json`.

**To run it:** Actions → `publish-vendor` → Run workflow. Run it once with
`dry_run` left `true` (checked into the input default) to exercise the
whole path — build, version check, `npm publish --dry-run` — without
actually publishing, then run it again with `dry_run` set to `false`.

**When to run it again:** only when the llama.cpp pin in
`packaging/make-vendor-package.sh` changes (a new commit means a new
package version, per the `PKG_VERSION` format above), not on every push —
the version-check step makes an accidental extra dispatch harmless either
way.

**Until it's published**, `npm ci`/`npm install` fails on a clean checkout;
`npm run vendor` is the supported local workaround, and it calls this script
for you (see
["`npm run vendor` — the local fallback"](#npm-run-vendor--the-local-fallback)
above).

**After the first publish**, run `npm install` once in a clean checkout and
commit the `package-lock.json` it generates, so that `npm ci` gives
reproducible, lockfile-pinned installs going forward. No `package-lock.json`
is committed yet.
