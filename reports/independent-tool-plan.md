# Making Onyx Engine an independent tool

A plan to make Onyx Engine easy to develop around and easy to use outside the JX ecosystem, while
keeping JX Runtime as the best-supported integration. Written 2026-10-22 from a review of the repo
(README, ROADMAP, CHANGELOG, `docs/`, `src/`, `packaging/`, `.github/`). It adds to
[`ROADMAP.md`](../ROADMAP.md) and does not replace it; where an item is already on the roadmap, the
roadmap entry is named.

Nothing here has been built or run. Claims about the repo come from reading it; claims about JX
Runtime are unverified, because its repo was out of reach for the review (see
[Things to confirm](#things-to-confirm)).

## Decisions made

1. **Audience: embedders.** People putting a local model server inside their own app or service, and
   people who want decision models (`/v1/systemone`). Not desktop users who want model pulling, a
   UI or automatic GPU setup; those stay out of scope, like multi-model routing, MCP and a web UI
   already are.
2. **Independent, best with JX.** Onyx Engine is a tool anyone can use. JX Runtime remains the
   first-class integration, documented as such, and gets no private contract: anything it relies
   on is a public, documented, stable behaviour.
3. **A Node-free build is acceptable** to JX Runtime, subject to the check in
   [Things to confirm](#things-to-confirm).

## Where things stand

The coupling to JX is shallow in code and deep in packaging, docs and positioning.

| Area | Finding |
|---|---|
| Code | Only `src/args.cpp` (the `--help` banner says "for JX Runtime"), the `CMakeLists.txt` and `package.json` descriptions, and a few comments name JX. The server is already a plain OpenAI-compatible server. |
| Build | A C++ project needs Node: `npm ci` runs `scripts/install-vendor.js`, which sparse-fetches llama.cpp from the `packaging/vendor/llama.cpp.conf` pin into `node_modules/@jxburros/llama-cpp-source`. `package.json` is `@jxburros/onyx-engine`, `"private": true`. |
| Releases | Prebuilt binaries are CPU-only; x64 builds need AVX2. No arm64 Linux build. |
| Distribution | No Dockerfile, Homebrew formula, winget manifest or install script in the repo. |
| Positioning | The README's "Why it exists" is JX Runtime; the binary's `--help` banner and the CMake/npm descriptions say "for JX Runtime". |
| Tests | One bash + Python smoke script (`scripts/smoke-test.sh`) and a conversion test. The smoke suite skips on Windows. No `ctest` target, no C++ unit tests. |
| Contributing | No `CONTRIBUTING.md`, issue templates or PR template (`.github/` holds `dependabot.yml` and four workflows). `docs/development/` is written for maintainers. |
| Integration surface | HTTP only. No supervisor guide, no client examples for non-JX callers. |
| Evidence | No published benchmark or conformance results against `llama-server` or the OpenAI client libraries. |
| Readiness | The port stays closed until the model loads, so a closed port is ambiguous between "loading" and "crashed". `--port` rejects 0, so a supervisor cannot ask for a free port and read it back. |

## Work items

Ordered. Items 1 and 2 are small, independent of each other and can start now.

### 1. Node-free build

**Why.** The largest single friction for an outside C++ developer: clone, install Node, run a
fetch script, then build.

**Do.**
- Make `cmake -B build && cmake --build build` work from a bare clone. Fetch the pinned llama.cpp
  commit with CMake (`FetchContent` with the commit as `GIT_TAG`, or a submodule); decide which
  while implementing, and keep the existing resolution order (`ONYX_ENGINE_LLAMA_DIR`, then the
  fetched tree, then `vendor/llama.cpp`) so offline and override workflows survive.
- Keep the single source of truth for the pin. `packaging/vendor/llama.cpp.conf` is read by four
  tools today (`make-vendor-package.sh`, `vendor.sh`, `install-vendor.js`, `CMakeLists.txt`); do
  not add a fifth copy of the commit.
- Keep `npm ci` and `npm run build` working as thin wrappers over the CMake path, so anything that
  calls them keeps working.
- Add `CMakePresets.json` (release, debug, and one per backend: CUDA, Vulkan, Metal, HIP).
- Update `docs/getting-started.md`, `docs/development/building.md` and
  `docs/development/vendoring.md`.

**Done when.** A fresh clone with only CMake, a compiler and Git builds and passes `--version`;
`npm ci && npm run build` still produces the same binary; CI covers both paths; the vendored
commit is verified as it is today.

### 2. Reposition the project

**Why.** A stranger should learn what Onyx Engine is before learning who spawns it.

**Do.**
- Rewrite the README opening: what it is for anyone (a small, strict, supervisable
  OpenAI-compatible server with decision-model support). Move JX Runtime to a "Works best with JX
  Runtime" section that links `docs/integration/jx-runtime.md`.
- Add a "Using it with other clients" section: the `openai` Python and JS clients, Open WebUI,
  Continue and a plain `curl`, each with the base URL and what to expect from strict field handling.
- Change the `--help` banner and the `CMakeLists.txt` and `package.json` descriptions to drop
  "for JX Runtime". Keep `--help` and the parser moving together, per `docs/development/README.md`.
- Reframe `src/args.h`/`args.cpp` comments about JX's adapter: the flags JX passes are
  llama-server compatibility flags, and the stable `--help` is a feature for any supervisor.
- Lead with decision models as the differentiator; link `docs/guides/decision-models.md`.

**Done when.** The README's first screen contains no JX reference; a first-time reader can find
the supported clients and the limits without reading the architecture notes.

### 3. Stability policy

**Why.** "Independent" means other people can build on it without surprises. It also gives JX
Runtime the same guarantee instead of a private one.

**Do.** Add `docs/reference/stability.md` stating, per area, what is stable across minor versions
and what can change: CLI flags and `--help` (with the "in `--help` means accepted" rule), routes
and response shapes, `/health` semantics, exit codes, the `usage` and `timings` frames, and the
llama.cpp pin. Say how breaking changes are announced (changelog section, version bump rule).
Link it from the README and the JX integration doc.

**Done when.** The page exists, the changelog references it, and the JX integration doc lists
only behaviour that appears on the stability page.

### 4. Supervising Onyx from your own app

**Why.** Embedders are the target audience, and today the supervision contract is only written
down from JX Runtime's side.

**Do.** Add `docs/guides/supervising.md`: spawn arguments, probing `--help`, waiting for ready,
`/health` and `/props`, graceful shutdown, exit codes, the stderr lines to expect, choosing a
free port, concurrency settings (`-c`, `-np`) and their per-slot effect, and short examples in
Node, Python and shell. Link it from `docs/guides/README.md`.

**Done when.** Someone who has never seen JX Runtime can write a working supervisor from the page.

### 5. Readiness and port selection (changes Onyx and JX together)

**Why.** Today a closed port means "loading or crashed". A supervisor in any language needs to
tell the two apart, and a free-port workflow needs a way to learn which port was bound. This is
roadmap decision 6 and the "Readiness" item under Later, brought forward because of the new
audience.

**Do.**
- Listen immediately and answer `503` with a structured body (`loading`) from `/health` and the
  model routes until the model is up; `200` when ready. Decide the exact body, then document it
  on the stability page.
- Allow `--port 0` and report the bound port on stderr and in `/props`. (`--port` rejects values
  below 1 today.)
- Change JX Runtime's `_waitReady()` in the same release cycle; because both are controlled by the
  same owner, version the change and note it in both changelogs rather than freezing the old
  behaviour.
- If a transition period is wanted, consider a flag that keeps the old behaviour; decide after
  checking how JX Runtime polls.

**Done when.** The smoke test covers loading, ready and failed-load; the JX integration doc and
README describe the new behaviour; JX Runtime's change is released alongside.

### 6. Operator features

**Why.** Embedders run the engine unattended. These are roadmap "Now" items; the audience makes
them the first features to build rather than the first to defer.

**Do.** In this order, as separate changes: `X-Request-Id` echo; `--max-queue` answering `503`
with `Retry-After`; `/metrics` under llama.cpp's `llamacpp:*` names; a per-tick token budget
(decodes first). Also the hardening items already on the roadmap: refuse a non-loopback `--host`
without `--api-key`, constant-time key compare, a loopback Host allowlist, an explicit payload cap.

**Done when.** Each has a smoke-test case, a CLI reference entry and a compatibility-page note.

### 7. Run it in one command

**Why.** An embedder shouldn't need a toolchain just to try it, and most local-model users have a
GPU.

**Do.**
- A Vulkan prebuilt for Linux and Windows (the cheapest portable GPU option), a baseline x64
  build that does not need AVX2, and an arm64 Linux build. Pick CUDA variants separately.
- A Docker image: CPU first, then CUDA; a documented one-line run with a mounted models directory.
- Defer Homebrew and winget until releases are stable and the first-release dry run is done.
- Windows signing and macOS notarization stay on the roadmap's open decisions.

**Done when.** `docker run` serves a model from a documented command; the release page offers a
GPU-capable archive; checksums continue to ship.

### 8. Contributor loop

**Why.** The architecture docs explain the engine to maintainers; outside contributors need a
short path from clone to passing test.

**Do.**
- `CONTRIBUTING.md` with a ten-minute path: build, run the tests, make and verify a change, the
  docs and version rules from `docs/development/README.md`.
- A `ctest` target with a small C++ test harness for argument parsing, request validation and the
  converter, so these do not need a model or a running server.
- A page on where code lives (`engine.cpp`, `server.cpp`, `decision.cpp`, `args.cpp`, `convert.cpp`)
  and how a request moves through them; build on `docs/architecture/README.md#source-layout`.
- Issue templates (bug, feature, question) and a PR template.
- Get the smoke suite running on Windows, or document why not.
- Mark a handful of issues "good first issue".

**Done when.** A new contributor can build, run the unit tests, and open a PR without asking
questions in the issue.

### 9. Evidence

**Why.** "Strict, predictable and fast enough" is a claim until it is measured.

**Do.**
- A conformance suite that points the official OpenAI Python and JS clients at every route.
- A benchmark page against `llama-server` on the same build: time to first token, tokens per
  second at `-np 1/2/4`, memory, and prefix-reuse savings. Publish methodology and commands.
- Run the engine against real models in CI at least nightly (the smoke suite uses tiny generated
  models, which proves wiring, not behaviour).
- Land the roadmap's ASan/UBSan job and fuzz harnesses.

**Done when.** The README links a benchmark and a conformance report that anyone can rerun.

## Explicitly not now

- Model pulling, a UI, automatic GPU detection or other desktop-style features.
- `/v1/responses` and `/v1/messages` (agent-tool audience); stay on the roadmap's Next list.
- Speculative decoding (Stage 4), Stage 3 chat-layer ownership and the converter work: still
  open decisions on the roadmap, none required for independence.
- Homebrew, winget (see item 7).

## Things to confirm

- **JX Runtime and the Node-free build.** This repo's docs say JX installs release archives and
  probes `--help`, not that it builds from source. Confirm that JX Runtime never runs `npm ci`
  inside an Onyx checkout. Keeping the npm wrappers (item 1) makes this safe either way.
- **JX Runtime's `_waitReady()`** polling behaviour, before fixing the readiness body (item 5).
- **Release tooling.** `release.json` and `.release/` come from a shared release system; confirm
  that changing package names or removing the npm scope from the main path does not break it.
- **Version policy.** The `every-main-change` rule means each change here advances the
  onyx-engine version (`package.json` and `release.json`), including docs-only changes.

## Suggested order

1. Items 1 and 2 (independent; separate commits).
2. Item 3, then item 4.
3. Item 5, coordinated with JX Runtime.
4. Item 6, then item 7.
5. Items 8 and 9, which can run alongside the rest.

## Checklist

Items 1 to 8 landed in 0.6.0 (see the [changelog](../CHANGELOG.md)). Answers to
[Things to confirm](#things-to-confirm), found while doing them: JX Runtime never runs `npm ci` in
an Onyx checkout (it installs release archives); its `_waitReady()` treats only `res.ok` as
ready and retries on `503`, so early listening needed no JX change (its next release records
the first `503` as a load stage); the release tooling keeps the npm package names, which item 1
did not change; and 0.6.0 is a minor advance because it carries breaking changes. Two
departures from the plan: the CMake fetch is a Git sparse fetch (not `FetchContent`) placed in
`.deps/`, and the stale `v0.4.1`/`v0.5.1` drafts still need deleting by hand.


- [x] 1. Node-free CMake build with npm wrappers kept, presets added
- [x] 2. README, `--help` and descriptions repositioned; client section added
- [x] 3. Stability policy page
- [x] 4. Supervising guide
- [x] 5. Early listen with `503 loading`; `--port 0`; JX `_waitReady()` updated
- [x] 6. `X-Request-Id`, `--max-queue`, `/metrics`, hardening
- [x] 7. Vulkan and baseline builds, arm64 Linux, Docker image
- [x] 8. `CONTRIBUTING.md`, `ctest`, templates, Windows smoke
- [ ] 9. Conformance suite, benchmarks, real-model nightly, sanitizers and fuzzing
