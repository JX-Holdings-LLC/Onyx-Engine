[Documentation](../README.md) › [Development](README.md) › Testing and CI

# Testing and CI

Everything here runs offline against tiny generated models. No test ever downloads weights.

## Unit tests (ctest)

`tests/unit/` holds a small C++ suite that needs no model and no server: argument parsing
(`src/args.cpp`), the request helpers (`src/http_util.cpp`: media payloads, base64, `X-Request-Id`,
the API-key compare, the `Host` allowlist) and model-path resolution (`src/convert.cpp`, without
running the converter). The harness is one header, `tests/unit/check.h`, with no dependency.

```bash
cmake --build build --target onyx-unit-tests
ctest --test-dir build --output-on-failure          # add -C Release with Visual Studio
build/onyx-unit-tests http                          # one suite, verbose
```

ctest has one entry per suite (`unit.args`, `unit.convert`, `unit.http`). The tests build by
default when Onyx is the top-level project (`ONYX_ENGINE_TESTS`). A release build with
`--target onyx-engine` does not build them.

## Test scripts

Both test scripts run fully offline: every model they need is generated
locally by a small numpy-only script, so nothing is downloaded from a model
hub and CI needs no external model access.

| Script | Exercises | Test model generator |
|---|---|---|
| `scripts/smoke-test.sh` | CLI `--help`/`--version` and argument refusals, all HTTP endpoints, streaming, parallel slots, context shift, `--mmproj` multimodal chat, logprobs, `--reasoning-budget`, rank pooling, base64 embeddings, `/v1/systemone` on a decision model, readiness (`503` while loading, a failed load, `--port 0`) and operations (auth, `Host` allowlist, `X-Request-Id`, `/metrics`, `--max-queue`, `--tick-tokens`) | `scripts/make-tiny-model.py` (tiny GGUF: generation, embedding and, with `--decision`, an openjev-style decision model), `scripts/make-tiny-mmproj.py` (tiny llava-style projector GGUF) |
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

Every leg builds, runs `onyx-engine --version`, the unit tests and `scripts/smoke-test.sh`.
The `linux` and `macos` legs build with `ccache` and also run `scripts/safetensors-test.sh`. The
`windows` leg runs the smoke suite under Git Bash, against `build/Release/onyx-engine.exe`: the
script falls back to `python` where there is no `python3`, and hands the native binary Windows
paths. `.gitattributes` keeps `*.sh` and `*.py` at LF endings so the checkout runs. The Windows
leg first existed so an MSVC toolchain break is caught on a pull request rather than during a
release (the first `v0.3.0` release run failed at exactly that step; see
[releasing](releasing.md#lessons-from-the-first-release-attempt)).

The `build-paths` job proves the two ways to build from a clone: `cmake-only` removes Node from
the runner, configures with the `release` preset (CMake fetches llama.cpp into `.deps/`), checks
the fetched commit against the pin and runs `--version`; `npm` runs `npm ci && npm run build`.

## The nightly QA profile

[`.ai/qa.yaml`](../../.ai/qa.yaml) and [`.ai/qa.md`](../../.ai/qa.md) tell the QA runner what to check for a native binary. Every executable check goes through `node scripts/tools/qa-check.js <check>`, which works under `cmd.exe` as well as `sh` and prints `QA_CHECK_SKIP: <reason>` when a prerequisite (cmake, Python with numpy, a POSIX shell) is missing.

| Levels | What runs |
|---|---|
| 1 Install health | `npm ci`, then confirm the vendored source carries the pinned commit |
| 2 Test health | `npm test` on a fresh build |
| 3 Build health | `npm run build` in Release, then `--version` |
| 4 Runtime health | start the binary on the tiny model, wait for `/health`, list `/v1/models`, make one chat completion |
| 5 AI review | advisory; never gates the others |
| 6, 7, 12 | built-in dependency, documentation and secrets/workflow audits |
| 8-11, 13 | browser levels: SKIP, never FAIL. Onyx Engine has no web UI |

Use only generated tiny models (random weights, so only response shapes are checked) and bind test servers to `127.0.0.1` on free ports.
