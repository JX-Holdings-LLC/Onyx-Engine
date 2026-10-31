# Onyx Engine action items from the 2026-10-31 cross-repo review

**Date:** 2026-10-31 · **Agent:** Claude Code (Claude)

**Source:** the cross-repo review of Media Runtime, Media Engine, JX Runtime and Onyx Engine,
committed in Media-Runtime as
[`docs/reviews/2026-10-31-cross-repo-review.md`](https://github.com/jxburros/Media-Runtime/blob/main/docs/reviews/2026-10-31-cross-repo-review.md)
(on branch `claude/lucid-feynman-vean0r` until merged). Section references below (§N) point to that
document.

This is the to-do list for **Onyx Engine**. It covers what the review suggests adopting from Media
Engine, the decisions only the maintainer can make, and the inconsistencies it found here.

Like the other reports in this folder, it is dated analysis and is not edited when the code moves.
The review **read the code and did not run it**. [`ROADMAP.md`](../ROADMAP.md) decides what is
scheduled; nothing here is scheduled until the roadmap says so.

Priority means:

- **P1** helps JX Runtime the most and carries little risk.
- **P2** is valuable.
- **P3** is later.

Items marked **(JX)** have a matching item in
[JX Runtime's action items](https://github.com/jxburros/JX-Runtime/blob/main/docs/audits/2026-10-31-cross-repo-review-action-items.md).
Items marked **(decision)** wait on a decision in section C.

## A. P1: a contract JX Runtime can check

Today there is no protocol version or negotiation. Clients discover capabilities from `--version`,
`--help` probing and `GET /props`, under the rules in
[`docs/reference/stability.md`](../docs/reference/stability.md). Media Engine advertises a versioned
protocol and ships a conformance suite that runs against any worker (§7.1, §8).

- [ ] **A1. A machine-readable capability document.** Either `GET /v1/capabilities` or a `capabilities`
  block in `/props`. It should carry:
  - a contract version string;
  - the process mode (text, embedding or decision);
  - the supported request fields and their limits;
  - the server-side limits (body size, JSON nesting depth, queue).

  The stability page already defines the compatibility rules; this gives them a machine-readable
  form. Ties to the `--help`-is-the-contract rule. **(JX A1)** **(decision C1)**
- [ ] **A2. A conformance suite that runs against any URL.** For example `tests/conformance/`, using
  standard-library Python like Media Engine's. It covers:
  - readiness and the 503 "Loading model" phase;
  - auth, Origin and Host refusals;
  - every route's shape;
  - streaming frames, `usage`/`timings` and `[DONE]`;
  - queue limits and `Retry-After`;
  - cancellation on client disconnect;
  - the error envelope.

  Run it in CI on the tiny models next to `scripts/smoke-test.sh`, and publish it so JX Runtime's CI
  can run it against the pinned release. This is the conformance suite named in
  `reports/independent-tool-plan.md` item 9. **(JX A2)**
- [ ] **A3. Graceful shutdown.** Handle SIGTERM and SIGINT:
  - stop admission and answer 503;
  - let in-flight requests finish up to a drain deadline;
  - then exit.

  Add an opt-in supervised-stdin watch so an orphaned engine exits when JX goes away. Today there is
  no signal handler and SIGTERM drops in-flight requests (`docs/guides/supervising.md`
  "Stopping"). (§8)
- [ ] **A4. Opt-in JSON startup record.** `--startup-json` prints one stdout line with version, llama.cpp
  build, port, PID and a random session ID. Keep the two stderr lines for existing supervisors. (§8)
- [ ] **A5. Model digest check.** `--model-sha256 HEX` refuses to load a file that does not match. JX
  already records each model's sha256. (§8)
- [ ] **A6. Compatibility record.** List Onyx version × JX version × conformance-suite version × result ×
  hardware. Keep it here or in JX Runtime. **(JX A3)**

## B. P2–P3

- [ ] **B1 (P2). Memory estimate on `/props`.** Compute it from GGUF tensor metadata plus the KV formula
  for the configured context, slots and cache types, and state its basis. This is the groundwork for
  the roadmap's `--fit`. JX's `src/estimate.js` could then defer to the engine. (§8)
- [ ] **B2 (P2). Benchmark-gated defaults.** Advertise an optimization, or change a default, only with a
  cited benchmark that records improvement, determinism and a quality note. Apply this to
  `--tick-tokens`, KV quantization, flash attention, and later speculative decoding. The first
  benchmark page is roadmap "Evidence" item 9. **(JX B4)**
- [ ] **B3 (P2). Accelerated archives.** Ship CUDA archives and push the CUDA image. The CUDA target
  already exists in `Dockerfile`. This is open roadmap decision 5. JX marks every Onyx build
  `accelerated: false` until this exists. **(JX A5)**
- [ ] **B4 (P3). Mutual TLS network mode.** Only if JX Runtime adopts remote engines (JX decision C1).
  Otherwise keep TLS at a proxy.
- [ ] **B5 (P3). Shared release convention.** The roadmap's planned `docs/release-convention.md` should
  also cover Media Engine's native archives, since they use the same release infrastructure.

## C. Decisions for the maintainer

- [ ] **C1. Contract versioning.** Choose between:
  - a separate contract version string, like `media-worker/1.3`;
  - the engine's own semver with the stability page's rules.

  Also decide where the capability document lives: `/props` or a new route.
- [ ] **C2. Conformance suite location.** Keep it in this repository, with JX consuming it, or in JX
  Runtime?
- [ ] **C3. Governance.** Onyx is agent-maintained (changelog entries are signed "Agent: Claude Code
  (Claude)") but has no `AGENTS.md`. Adopt Spec-Driven Docs as JX Runtime did, or add only an
  `AGENTS.md` that points to `CONTRIBUTING.md`?
- [x] **C4. Version bump.** Decided 2026-10-31: patch bump to 0.7.1 for this report, under the `every-main-change` policy.

## D. Inconsistencies found

- [ ] **D1.** `.ai/qa.md:58-59` says "On Windows the smoke suite SKIPs … CI runs it on Linux and macOS".
  [`ci.yml`](../.github/workflows/ci.yml) now runs it on `windows-latest` under Git Bash (comment at
  line 51).
- [ ] **D2.** JX Runtime pins Onyx `v0.6.0` (`JX-Runtime/src/binaries.js:302`) while this repository is
  at 0.7.0, and the roadmap says the unified release workflow has not yet published a release.
  Publishing 0.7.0, after its dry run, unblocks JX's pin bump. **(JX A4)**
- [ ] **D3.** No change needed. `research_notes/…/serving_techniques.md` and `models_quant_conversion.md`
  cite `docs/architecture.md`, which is now the `docs/architecture/` folder. The notes are dated
  evidence, and by this folder's convention they are not edited. Listed only so nobody "fixes" them
  by mistake.
- [ ] **D4.** The link check documented in
  [`docs/development/documentation.md`](../docs/development/documentation.md) ("Checking links")
  exits 1 on `main`, reporting 9 missing targets. None of them comes from this report. They are:
  - Links written with `%20`, such as those in [`reports/README.md`](README.md),
    `docs/architecture/kv-cache.md` and `docs/architecture/README.md`. They resolve on GitHub, but
    the script does not URL-decode them.
  - Absolute `/home/user/Onyx-Engine/…` links, and one `…` placeholder link, in
    `research_notes/Replacing llama cpp common code/` (`kv_cache_state.md`,
    `chat_templates_tools.md`). These are broken on GitHub too.

  Either have the script apply `urllib.parse.unquote`, or exclude `research_notes/`, or both. The
  notes themselves are dated evidence (see D3).
