# QA instructions: Onyx Engine

Onyx Engine is a C++ binary, not a web app. One `onyx-engine` process loads one
GGUF model and serves an OpenAI-compatible HTTP API on `127.0.0.1`; JX Runtime
spawns it per loaded model and proxies callers to it
([docs/jx-runtime-integration.md](../docs/jx-runtime-integration.md)). It is
built with CMake against llama.cpp, vendored at the commit pinned in
`packaging/vendor/llama.cpp.conf` and fetched by `npm ci`. npm is only the
installer and task runner; the package has no npm dependencies.

The executable levels are in [`qa.yaml`](qa.yaml). Every check runs through
`node scripts/tools/qa-check.js <check>`, which works under `cmd.exe` and `sh`
alike and prints `QA_CHECK_SKIP: <reason>` when a prerequisite is missing on
the runner.

**What each requested level is expected to do** (the app runs with
`qa_profile: full`, which requests levels 1–13):

| Levels | Expected outcome | Why |
| --- | --- | --- |
| 1–4 | Run, as defined in `qa.yaml` | Install, smoke tests, build, and serving a request are what matter here |
| 5 | Advisory | AI review never gates the deterministic levels |
| 6, 7, 12 | Built-in defaults, with the notes below | Dependency, documentation, and secrets/workflow checks still apply |
| 8–11, 13 | SKIP, never FAIL | Browser-required; Onyx Engine has no browser UI (`--no-webui` is accepted only for llama-server compatibility) |

A SKIP on levels 8–11 and 13 is the correct result. The cleanest way to get it
is `browser_qa: { enabled: false }` on this app's entry in the QA runner's
`config/apps.yml`. Do not add a browser UI or fake page checks to make those
levels pass.

## Ground rules

- Use only the tiny models generated into `models-test/` by
  `scripts/make-tiny-model.py` and `scripts/make-tiny-mmproj.py`. Never
  download real models.
- Test servers bind `127.0.0.1` on free ports and are stopped when a check ends.
- A missing toolchain on the runner (cmake, a C++ compiler, Python with numpy)
  is a provisioning gap. Report it as SKIP or blocked with the reason, not as a
  code failure.
- `vendor/`, `node_modules/@jxburros/llama-cpp-source/` and `third_party/` are
  vendored code. Do not review or lint them as Onyx code.

## 1 - Install Health

- `npm ci` must succeed. Its postinstall (`scripts/install-vendor.js`) fetches
  llama.cpp with git at the pinned commit; it does not use the npm registry.
  A failure here usually means git cannot reach GitHub.
- `qa-check.js vendor` must confirm `node_modules/@jxburros/llama-cpp-source`
  carries the pinned commit in `.vendor-source-commit` and has its required
  files. FAIL on a mismatch.

## 2 - Test Health

- `npm test` is `bash scripts/smoke-test.sh`, which needs a built binary,
  bash, curl and python3 with numpy, pyyaml, tqdm and requests (for the
  vendored gguf-py that writes the test models). The level builds first, so it
  does not depend on level 3 having run.
- On Windows the smoke suite SKIPs: it is POSIX shell, and CI runs it on Linux
  and macOS. Level 4 still exercises the Windows binary.
- `scripts/safetensors-test.sh` is a separate conversion test that CI runs; it
  is not part of this level.

## 3 - Build Health

- `npm run build` (`scripts/build.js`) configures CMake and builds only the
  `onyx-engine` target, in Release, with a bounded job count. A cold build
  compiles llama.cpp and takes several minutes.
- The binary lands in `build/onyx-engine`, or `build/Release/onyx-engine.exe`
  with Visual Studio. `--version` must print `onyx-engine`.
- FAIL on a configure or compile error. SKIP only when cmake is not installed.

## 4 - Runtime Health

- This is the runtime surface: start the binary on the tiny model, wait for
  `GET /health` to answer 200 (the server listens only once the model has
  loaded, so earlier connections are refused), check
  `GET /v1/models` lists the alias, and make one short chat completion.
- The output is from a random-weight model, so only the response shape is
  checked, never the text.

## 5 - AI Code Review

- Advisory only. Without `OPENAI_API_KEY` the level is blocked, and findings
  never fail the run.
- Focus on request parsing and validation in `src/server.cpp`, slot and batch
  concurrency in `src/engine.cpp`, and agreement with `docs/api.md`.
- `logit_bias` is rejected with a 400 on purpose. Decision-model support
  (`/v1/systemone`) is not implemented yet (issue #49). Neither is a defect to
  report.

## 6 - Dependency & Security Audit

- There are no npm dependencies. The dependency that matters is the pinned
  llama.cpp (and ggml) commit in `packaging/vendor/`. `scripts/check-ggml-pin.sh`
  checks the two pins stay together.

## 7 - Project Documentation Audit

- `README.md`, `docs/api.md`, `docs/building.md` and
  `docs/jx-runtime-integration.md` are the user-facing documents; they should
  agree with the flags in `src/args.cpp` and the endpoints in `src/server.cpp`.

## 12 - Secrets & Workflow Audit

- Workflows are in `.github/workflows/`; shell scripts in `scripts/` and
  `packaging/` are bash. Release workflows call the shared release
  infrastructure at a pinned commit; flag any unpinned third-party action.
