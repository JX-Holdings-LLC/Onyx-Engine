# Contributing to Onyx Engine

Thanks for helping. This page gets you from a clone to a passing test and an open pull request
in about ten minutes of your time (plus one cold build of llama.cpp). Nothing here assumes you
use JX Runtime.

## 1. Build

You need CMake 3.16 or later, a C and C++17 compiler, and Git. Node.js is optional.

```bash
git clone https://github.com/JX-Holdings-LLC/Onyx-Engine.git
cd Onyx-Engine
cmake -B build -DCMAKE_BUILD_TYPE=Release        # fetches the pinned llama.cpp into .deps/
cmake --build build -j 8                         # the binary and the unit tests
./build/onyx-engine --version
```

Give `-j` a number: a bare `-j` starts every llama.cpp file at once and can exhaust memory. The
first build compiles llama.cpp and takes several minutes; later builds compile only what you
changed. `cmake --preset debug` (CMake 3.21+) gives a debug build in `build/debug/`. With Node,
`npm ci && npm run build` is equivalent. See [building](docs/development/building.md) for GPU
backends.

## 2. Run the tests

```bash
ctest --test-dir build --output-on-failure       # C++ unit tests, about a second
bash scripts/smoke-test.sh                       # end to end, a few minutes
```

The unit tests need nothing else. The smoke test needs Python 3 with numpy, and curl; it
generates tiny random-weight models, starts several engines on ports 18190 to 18202 and checks
the shape of every response (the words are random). `ONYX_SMOKE_PORT` moves the ports. On
Windows, run it from Git Bash with the binary's path: `bash scripts/smoke-test.sh build/Release/onyx-engine.exe`.
`bash scripts/safetensors-test.sh` covers the converter (Linux and macOS).

## 3. Make and verify a change

1. Find the code: the [code map](docs/development/code-map.md) lists every file and follows one
   request through them.
2. Make the change. Match the surrounding style; comments explain why, not what.
3. Add a test where it fits:
   - a unit test in `tests/unit/` for parsing or a pure helper (`TEST(suite, name)` with `CHECK`
     and `CHECK_EQ`; see `tests/unit/check.h`);
   - a `check` in `scripts/smoke-test.sh` for anything visible over HTTP or on the command line.
4. Run both test suites.
5. Update the docs in the same change (see below).

## 4. The rules every change follows

These come from [`docs/development/README.md`](docs/development/README.md):

- **The parser and `--help` move together.** A new flag goes into `src/args.cpp`'s parser and
  its help text in the same change, or into neither. Supervisors probe `--help` to learn what a
  build accepts.
- **Refuse rather than ignore.** An unsupported request field gets a `400`, not silence.
- **Docs move with the code.** A flag goes in [`docs/reference/cli.md`](docs/reference/cli.md),
  a field or route in the [API pages](docs/reference/api/README.md), a mechanism in the
  [architecture notes](docs/architecture/README.md). [Documentation](docs/development/documentation.md)
  lists the rest.
- **Stability.** If your change alters anything on the [stability page](docs/reference/stability.md),
  say so in the pull request; it needs a minor version and a **Breaking** changelog heading.
- **Every change advances the version**, docs-only changes included (the `every-main-change`
  policy). A maintainer can do this for you before merging; if you want to do it yourself, see
  [releasing](docs/development/releasing.md): `release.json`, `package.json` and
  `package-lock.json` move together, with a notes file in `.release/notes/onyx-engine/` and a
  changelog line.

## 5. Open the pull request

Fill in the template: what changed, why, how you tested it. CI builds and tests on Linux, macOS
and Windows, and checks the version advance. Small, focused pull requests are reviewed fastest.

## Reporting bugs and asking questions

Use the issue templates. For a bug, include `onyx-engine --version`, the exact command line, the
request (with secrets removed) and the stderr tail. Security problems go to the maintainers
privately, not in a public issue.

## Licence

Contributions are accepted under the project's [MIT licence](LICENSE).
