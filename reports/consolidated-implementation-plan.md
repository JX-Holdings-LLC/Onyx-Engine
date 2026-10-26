# Onyx Engine consolidated implementation plan

Snapshot: 2026-10-26 UTC. This is the active implementation index for the remaining repository audit, superseding older future-work lists for sequencing and disposition. Historical evidence and shipped architecture remain in their source documents. This is a plan, not a claim that its code is implemented.

Read the [roadmap](../ROADMAP.md) for release direction and the [owner decision register](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md) before changing policy. Decisions are proposals until explicitly recorded as accepted.

This plan preserves **109 retained audit entries** with stable IDs, including overlapping issues, verification and conditional research. It accounts for every open issue after the closure pass: **12 issues and four open dependency PRs. Do not treat the row count as an estimate of independent features.

## How to execute

1. Select an item in an eligible package; check its current issue and source against main before coding. QA suggestions still require reproduction/triage; inclusion here is not a qa-decision/implement label change.
2. Satisfy the package prerequisites, item-level local gates, owner decisions and any external contract. An integration wait does not block independent implementation explicitly allowed below.
3. Make one bounded change with fixtures/contract tests, docs and changelog. For behavior-changing research, first commit a decision and supported scope; preserve defaults until evidence supports promotion.
4. Run the appropriate validation and retain exact revisions, commands, model hashes/configuration and outcomes. Mark unavailable hardware/external evidence as pending.
5. Close an issue only when its acceptance is met. Update this plan, the dependency contract and decision record with the implementing PR/release and evidence; do not delete the original audit ID.

## Package sequence and implementation boundaries

### O0 — Reconcile QA, documentation and dependency PRs

**Order:** Now.
**Entry gate:** Current main plus issue bodies and release evidence.
**Code/document scope:** scripts/tools/qa-check.js, vendor/build scripts, README, release metadata and workflow dependency PRs.

Triage source findings against full code; fix substantiated parser/diagnostic issues; close false reports with evidence; update stale release/issue tables and merge dependency bumps through version validation.

**Exit gate:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

### O1 — Small API, hardening and operator controls

**Order:** First implementation tranche.
**Entry gate:** No cross-repo blocker to direct-engine work; resolve timeout/schema/optional label policy.
**Code/document scope:** src/args.cpp, server.cpp request parsing/routes, engine context flags, unit tests and vendored httplib.

Extract pure request parsing; add models metadata, embedding dimensions, KV/MoE controls and queue timeout; upgrade httplib and bounded HTTP settings; verify actual decision-image and strict-schema behavior.

**Exit gate:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

### O2 — Common Stage 1 and owned sampler seam

**Order:** After O1 parsing baseline; before speculation/full chat ownership.
**Entry gate:** Golden baseline at exact pin; do not redo already shipped batch helpers.
**Code/document scope:** src engine/chat/sampling helpers and build dependencies on common; tokenizer/log/JSON wrappers.

Replace token/log helpers, introduce owned sampler state and bounded ordered JSON, narrow reuse capability gates and isolate upstream chat behind a seam. Make one behavior-preserving extraction per PR.

**Exit gate:** Golden token IDs/logprobs/grammar/seed behavior and streaming outputs match the baseline; error/depth/cancellation limits hold; common dependency changes are measured, not inferred.

### O3 — Common Stage 2: cache, checkpoints and fit

**Order:** After O2 capability seams.
**Entry gate:** Verified llama state APIs at pin; owner decision on unstable fit fallback; hardware/model evidence.
**Code/document scope:** engine sequence memory, slot state, cache keys, admission and model-fit estimates.

Implement SWA/recurrent checkpoints, byte-limited RAM LRU, validated save/restore and media-aware matching; add single-GPU/CPU fit fallback and opt-in unified KV with explicit admission/failure rules.

**Exit gate:** np1/np3+ mixed workloads preserve correct outputs; incompatible state is rejected; memory ceilings/eviction/cancel are deterministic; prefix reuse and fit savings are measured on targeted model families.

### O4 — Client protocols and model metadata

**Order:** After O1; may overlap O2/O3.
**Entry gate:** Backend parsing contracts and DEC-X03 protocol boundaries.
**Code/document scope:** server routes, request/response translators, streaming state machines and compatibility reference.

Add rerank, reasoning/strict-tool conventions, Messages and Responses in separate PRs. Keep optional model-detail/FIM/native controls behind candidate decisions. Coordinate JX routing only for end-to-end use.

**Exit gate:** Official Python/JS clients parse all supported modes; refusal is explicit; streamed lifecycle/usage/tools/reasoning/cancel agree with non-streaming output; JX adapter acceptance is an integration gate, not a blocker to direct clients.

### O5 — Common Stage 3: chat-layer ownership

**Order:** Conditional; after O2 and explicit ownership decision.
**Entry gate:** DEC-O01; budget to maintain family templates/parsers and golden corpus.
**Code/document scope:** Jinja/schema converter, render/format seams, model-family tool/reasoning parsers and CMake common linkage.

Either retain the tested upstream chat seam, or copy the licensed engines and own parsers/fallbacks with golden fixtures. Disable LLAMA_BUILD_COMMON only after full parity.

**Exit gate:** Approximately 20 representative templates pass tools/reasoning/grammar stream and non-stream parity; unsupported families are disclosed; new upstream templates have an assigned maintenance path.

### O6 — Common Stage 4: speculative generation

**Order:** After O2 sampler; independent of full O5 ownership.
**Entry gate:** DEC-O02, memory reservations and rollback semantics; hardware benchmark baseline.
**Code/document scope:** engine batch verification, sampler commit/rollback, n-gram/draft contexts and acceptance metrics.

Implement verifier first, adaptive per-slot n-grams second, optional draft model third; handle accepted/rejected tails, stop/logprobs and recurrent snapshots. Keep MTP/EAGLE3/DFlash gated on API maturity/explicit experiment.

**Exit gate:** Exact correctness/rollback tests pass under multi-slot cancellation; memory/acceptance/latency are measured; low-acceptance modes disable safely; no unconditional speedup/default-on claim.

### O7 — Release, converter and distribution maintenance

**Order:** Parallel after small-contract baseline.
**Entry gate:** Distribution/converter/signing decisions and platform resources.
**Code/document scope:** release/vendor pipelines, converter, Docker/build presets and JX asset contract.

Automate smaller pin bumps; document release convention; expand chosen GPU builds; attest/sign/reproduce artifacts; fix converter scope with architecture fixtures and explicit HF ownership.

**Exit gate:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

### O8 — Conformance, robustness and performance evidence

**Order:** Build early; rerun after each affected phase.
**Entry gate:** Representative hardware/models and CI capacity.
**Code/document scope:** SDK suite, golden outputs, sanitizer/fuzz/static-analysis jobs, metrics and benchmark reports.

Add official-client contracts, ASan/UBSan/TSan, bounded fuzz corpora and nightly real-model coverage; publish same-build llama-server comparisons at np1/2/4. Separate source risk from reproduced defects.

**Exit gate:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

### O9 — NVIDIA profiling and measured scheduling

**Order:** Separate opt-in workstream.
**Entry gate:** DEC-X06 plus bounded profiling contract; baseline O8 and engine ownership.
**Code/document scope:** Nsight recipes, optional NVTX/props, CUDA presets and measured prefill/admission scheduling.

Implement O1–O4 branch packages in order: baseline profiling, bounded instrumentation, evidence-backed builds, then measured mixed-media scheduling changes.

**Exit gate:** Tracing overhead and privacy are bounded; old JX ignores unknown props safely; GPU build matrix and matched mixed workloads show correctness and measured changes.

### O10 — Research candidates awaiting promotion

**Order:** Unscheduled.
**Entry gate:** Decision or evidence trigger; no feature is accepted merely by being listed.
**Code/document scope:** Optional extra APIs, backend sampling, LoRA/video, grammar engines, caching/scheduling and package channels.

Run scoped experiments using current pin; resolve ownership/compatibility/cost; either promote to a bounded PR with tests or record deferral.

**Exit gate:** Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

## Cross-repo dependency contracts

The producer owns the deliverable. A consumer may proceed with fixtures while waiting, but cannot claim the integrated feature until the release/contract evidence exists. Hardware, credentials and maintainer choices are additional non-repository dependencies, identified separately.

### X02 — JX Runtime → Onyx Engine integration

**Required deliverable:** JX routing/capability/type/permission and cancellation support plus managed pin/asset recognition for each newly released Onyx feature.

**What waits / what can proceed:** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

### X03 — Song Deck → JX Runtime + Onyx Engine

**Required deliverable:** Real role schemas/prompts and acceptance cases, capable 8B+ model/device access, browser origins and second-device TLS/pairing walkthrough.

**What waits / what can proceed:** Both repos can add fixtures; end-to-end role/schema/pairing claims wait for Song Deck exercising those exact contracts and retaining results. Song Deck application logic remains there.

### X05 — QAI-ality ↔ JX Runtime / Onyx Engine

**Required deliverable:** QAI-ality config/apps.yml enables JX browser QA with a deterministic startup/auth contract and disables irrelevant browser QA for headless Onyx.

**What waits / what can proceed:** Repo-local test fixtures and qa-check diagnostics can proceed; automated browser-level completion waits for a successful QAI-ality run with the correct configuration.

### X08 — Onyx Engine ↔ BackTrack

**Required deliverable:** Coordinated ggml source pin update and check-ggml-pin.sh results against BackTrack packaging/vendor/ggml.conf.

**What waits / what can proceed:** Onyx pin upgrade can be reviewed independently; claiming cross-repo pin alignment waits for BackTrack change and validation. Current BackTrack state must be inspected then.

### X09 — Onyx Engine + shared release infrastructure → JX Runtime / audio apps

**Required deliverable:** Stable release convention, supported asset variants/layout, checksums, optional attestation identity/trust-root protocol and working publisher.

**What waits / what can proceed:** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

### X10 — AI Server Studio ↔ JX Runtime ↔ Onyx Engine

**Required deliverable:** Versioned execution-profile, setup-plan and safe profiling properties contracts (branch C1/C2/C3/C4/C6), explicit public/admin boundaries and unknown-field fallback.

**What waits / what can proceed:** Developer measurement recipes can proceed independently. Profile/setup UI integration waits for agreed schemas and Studio consumption; Onyx optional properties must not break old JX.

### X12 — llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables

**Required deliverable:** Stable or deliberately pinned API support and security fixes; especially MTP/EAGLE3/DFlash state/rollback, backend loading, extended fit/batch interfaces.

**What waits / what can proceed:** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

### X13 — JX Runtime → Onyx documentation cleanup

**Required deliverable:** JX #327 confirms no links to the four moved Onyx docs and a matching integration contract; this producer requirement is satisfied.

**What waits / what can proceed:** Onyx can evaluate retiring compatibility stubs after checking other consumers. JX is no longer a blocker; this does not authorize deleting unrelated docs.

## Detailed work items

### O0 work items

<a id="oe-pr77"></a>

#### OE-PR77 — build(deps): Bump actions/checkout from 4 to 7

**State:** Open PR. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** build(deps): Bump actions/checkout from 4 to 7. Review checks, comply with every-main-change version policy, and merge or explicitly decline.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `PR 77`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/pull/77).

<a id="oe-pr76"></a>

#### OE-PR76 — build(deps): Bump hendrikmuhs/ccache-action from 1.2.20 to 1.2.24

**State:** Open PR. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** build(deps): Bump hendrikmuhs/ccache-action from 1.2.20 to 1.2.24. Review checks, comply with every-main-change version policy, and merge or explicitly decline.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `PR 76`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/pull/76).

<a id="oe-pr75"></a>

#### OE-PR75 — build(deps): Bump docker/setup-buildx-action from 3 to 4

**State:** Open PR. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** build(deps): Bump docker/setup-buildx-action from 3 to 4. Review checks, comply with every-main-change version policy, and merge or explicitly decline.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `PR 75`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/pull/75).

<a id="oe-pr74"></a>

#### OE-PR74 — build(deps): Bump docker/build-push-action from 6 to 7

**State:** Open PR. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** build(deps): Bump docker/build-push-action from 6 to 7. Review checks, comply with every-main-change version policy, and merge or explicitly decline.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `PR 74`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/pull/74).

<a id="oe-i72"></a>

#### OE-I72 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-3\] `scripts/tools/qa-check.js`: This is reasonable, but when it fails the script currently has to infer the reason elsewhe

**State:** Open / suggestion. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** When Python discovery fails, report attempted Python candidates and missing dependencies in scripts/tools/qa-check.js.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `#72`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/72).

<a id="oe-i69"></a>

#### OE-I69 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-3\] `README.md`: This can make the introduction harder to scan and maintain, especially when implementation

**State:** Open / documentation. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Trim repeated marketing/capability claims in README and keep them consistent with implemented behavior.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `#69`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/69).

<a id="oe-i68"></a>

#### OE-I68 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-2\] `scripts/install-vendor.js`: The two modules appear intended to share this function, but the excerpt does not show an e

**State:** Open / changed issue scope. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Current body proposes replacing the fragile key-based RegExp vendor-spec parser with a line parser. The title alleges a missing readSpec export, but module.exports = { readSpec } exists; that part is stale.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `#68`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/68).

<a id="oe-i67"></a>

#### OE-I67 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-1\] `scripts/build.js`: If `ONYX_BUILD_JOBS` is set to a non-numeric string, it silently falls back to CPU count i

**State:** Open / suggestion. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Validate ONYX_BUILD_JOBS explicitly for invalid, zero and negative values or document intentional fallback; Number(env) || CPU-count currently hides invalid input.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `#67`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/67).

<a id="oe-i65"></a>

#### OE-I65 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-001\] Review AI-flagged unverified risks from code review

**State:** QA review / umbrella. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Review unverified request safety, synchronization and API risks against full source, add reproduction or dismiss unsupported findings. Overlaps #70 and #71.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `#65`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/65).

<a id="oe-i64"></a>

#### OE-I64 — \[QA\]\[Onyx Engine\]\[F-001\] binary_version failed

**State:** QA triage / not confirmed defect. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Investigate the nightly binary_version job and build artifact availability using qa-check.js binary. Published builds exist; missing binary in a QA environment is not evidence the engine cannot build.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X05.

- **X05: QAI-ality ↔ JX Runtime / Onyx Engine.** Repo-local test fixtures and qa-check diagnostics can proceed; automated browser-level completion waits for a successful QAI-ality run with the correct configuration.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `#64`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/64).

<a id="oe-d013"></a>

#### OE-D013 — Old empty drafts and publication prose

**State:** Reconcile / cleanup. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** v0.6.2 is published with nine archives and checksums. Remove or otherwise resolve the empty v0.4.1/v0.5.1 drafts and associated stale tag state after checking references; update first-release/no-dry-run claims.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `D013`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/.release/README.md).

<a id="oe-d067"></a>

#### OE-D067 — Disable irrelevant browser QA in QAI-ality

**State:** Cross-repo dependency. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Verify browser_qa.enabled:false for this headless engine in QAI-ality config/apps.yml so browser levels cleanly skip; roadmap lists this follow-up even though #46/#47/#51 are closed.

**Local sequence:** Follow O0 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X05.

- **X05: QAI-ality ↔ JX Runtime / Onyx Engine.** Repo-local test fixtures and qa-check diagnostics can proceed; automated browser-level completion waits for a successful QAI-ality run with the correct configuration.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record.

**Traceability:** original audit `D067`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

### O1 work items

<a id="oe-i62"></a>

#### OE-I62 — Accept `dimensions` on /v1/embeddings

**State:** Open. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Support dimensions 1..n_embd by truncating then L2-normalizing before float/base64 encoding; validate bounds and document Matryoshka limitations.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `#62`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/62).

<a id="oe-i61"></a>

#### OE-I61 — Unit-test the request parsing helpers (stop, max tokens, sampling)

**State:** Open. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Extract and unit-test parse_stop, max-token precedence and sampling parsers: max_completion_tokens > max_tokens/n_predict, null ignored, empty stop and default seed.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `#61`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/61).

<a id="oe-i60"></a>

#### OE-I60 — Add a --timeout flag for the HTTP read/write timeout

**State:** Open / policy decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add --timeout with integer N >= 1 for both read/write timeouts, help, tests and docs. Issue proposes default 600; roadmap asks for a shorter read timeout. Resolve that policy difference.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O05](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o05)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `#60`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/60).

<a id="oe-i59"></a>

#### OE-I59 — Report context length and modalities in /v1/models

**State:** Open. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Expose usable per-slot context length and input/output vision/audio modalities in /v1/models; add unit/smoke tests and documentation.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `#59`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/59).

<a id="oe-d001"></a>

#### OE-D001 — Label-token logit_bias

**State:** Optional decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Decide whether to allow label-token bias so chat models can express label decisions with enum constraints and logprobs; logit_bias is still rejected.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** [DEC-O04](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o04)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D001`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/docs/architecture/decision-models.md).

<a id="oe-d002"></a>

#### OE-D002 — Multimodal decisions

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Implement clef, lfm2-d1-omni and vllm-d3 decision images with media evaluated in the same batch as the text that reads it; replace current 501 with tested behavior.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D002`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/docs/architecture/decision-models.md).

<a id="oe-d003"></a>

#### OE-D003 — KV cache type flags

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Wire --cache-type-k/-v to libllama, validate backend/flash-attention combinations and update help/tests/docs.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D003`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d004"></a>

#### OE-D004 — MoE CPU expert offload

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Expose --cpu-moe, --n-cpu-moe and tensor placement -ot, with honest estimates and safe supported configurations.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D004`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d005"></a>

#### OE-D005 — Queue-wait deadline

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add bounded queue waiting and defined timeout/cancellation/error semantics; --max-queue and --tick-tokens already shipped.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O05](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o05)

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D005`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d006"></a>

#### OE-D006 — cpp-httplib upgrade

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Move the vendored HTTP library from 0.54.1 toward the researched 0.62 baseline after compatibility/security tests, and establish update tracking.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O05](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o05)

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D006`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d011"></a>

#### OE-D011 — Song Deck schema regression suite

**State:** Planned verification. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Run real deep/nested, enum, arrays and additionalProperties/strict schemas through json_schema to GBNF, through JX and directly; return clear errors for truncated JSON.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X03, X02.

- **X03: Song Deck → JX Runtime + Onyx Engine.** Both repos can add fixtures; end-to-end role/schema/pairing claims wait for Song Deck exercising those exact contracts and retaining results. Song Deck application logic remains there.
- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D011`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/docs/guides/structured-output-and-tools.md).

<a id="oe-d041"></a>

#### OE-D041 — Grammar resource budgets

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Bound schema/grammar size, complexity and compilation time; define rejection and cancellation behavior for adversarial inputs.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O06](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o06)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D041`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/sampling_grammar_fit_helpers.md).

<a id="oe-d077"></a>

#### OE-D077 — HTTP connection and keepalive budgets

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Consider bounding concurrent connections and HTTP thread pool relative to np, and tuning keepalive count/timeouts in addition to the planned read timeout and existing payload cap.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O05](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o05)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D077`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d082"></a>

#### OE-D082 — Metadata-aware pooling defaults

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Use model metadata/architecture to select last pooling where appropriate and avoid an unconditional mean fallback for Qwen3-Embedding without metadata; test against known embeddings.

**Local sequence:** Follow O1 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch.

**Traceability:** original audit `D082`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/models_quant_conversion.md).

### O2 work items

<a id="oe-d007"></a>

#### OE-D007 — Own token and log helpers

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Replace common tokenize/detokenize/token-to-piece and log-threshold helpers with Onyx-owned thin llama.h wrappers. Batch helpers already landed in the b11820 update.

**Local sequence:** Follow O2 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Golden token IDs/logprobs/grammar/seed behavior and streaming outputs match the baseline; error/depth/cancellation limits hold; common dependency changes are measured, not inferred.

**Traceability:** original audit `D007`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d008"></a>

#### OE-D008 — Own sampler

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Build the owned sampling chain and state/replay/reset contracts needed for normal and speculative generation, preserving penalties, grammar, seed and logprob behavior.

**Local sequence:** Follow O2 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Golden token IDs/logprobs/grammar/seed behavior and streaming outputs match the baseline; error/depth/cancellation limits hold; common dependency changes are measured, not inferred.

**Traceability:** original audit `D008`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d009"></a>

#### OE-D009 — JSON replacement and limits

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Use nlohmann ordered JSON with a bounded nesting/depth policy and preserve wire ordering/validation behavior; add regression coverage.

**Local sequence:** Follow O2 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Golden token IDs/logprobs/grammar/seed behavior and streaming outputs match the baseline; error/depth/cancellation limits hold; common dependency changes are measured, not inferred.

**Traceability:** original audit `D009`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d010"></a>

#### OE-D010 — Narrow reuse gate and capability probes

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Replace blanket SWA/recurrent prefix-reuse disablement with evidenced support checks; probe memory seq_rm semantics and introduce a thin chat seam before broader ownership changes.

**Local sequence:** Follow O2 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Golden token IDs/logprobs/grammar/seed behavior and streaming outputs match the baseline; error/depth/cancellation limits hold; common dependency changes are measured, not inferred.

**Traceability:** original audit `D010`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

### O3 work items

<a id="oe-d016"></a>

#### OE-D016 — Context checkpoints for SWA/hybrid reuse

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Port checkpoint placement/restore with correct SWA positions and recurrent state, only between decode batches; regression-test Gemma/gpt-oss/Qwen/Granite families.

**Local sequence:** Follow O3 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** np1/np3+ mixed workloads preserve correct outputs; incompatible state is rejected; memory ceilings/eviction/cancel are deterministic; prefix reuse and fit savings are measured on targeted model families.

**Traceability:** original audit `D016`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/kv_cache_state.md).

<a id="oe-d017"></a>

#### OE-D017 — Host-RAM prompt cache

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Implement byte-capped LRU cache, safe sequence-state snapshots, eviction/resume and cache accounting; benchmark cold vs warm reuse and allocation failures.

**Local sequence:** Follow O3 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** np1/np3+ mixed workloads preserve correct outputs; incompatible state is rejected; memory ceilings/eviction/cancel are deterministic; prefix reuse and fit savings are measured on targeted model families.

**Traceability:** original audit `D017`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d018"></a>

#### OE-D018 — Slot save and restore

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Expose bounded state save/restore with model/build fingerprint and compatibility validation; guard corrupted/incompatible state and lifecycle cleanup.

**Local sequence:** Follow O3 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** np1/np3+ mixed workloads preserve correct outputs; incompatible state is rejected; memory ceilings/eviction/cancel are deterministic; prefix reuse and fit savings are measured on targeted model families.

**Traceability:** original audit `D018`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d019"></a>

#### OE-D019 — Media-aware prefix matching

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Match text and image/audio chunks with media hashes and correct positional accounting, retaining only valid reusable prefix state under --mmproj.

**Local sequence:** Follow O3 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** np1/np3+ mixed workloads preserve correct outputs; incompatible state is rejected; memory ceilings/eviction/cancel are deterministic; prefix reuse and fit savings are measured on targeted model families.

**Traceability:** original audit `D019`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d020"></a>

#### OE-D020 — Single-GPU plus CPU auto-fit

**State:** Planned / unstable API. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Implement --fit using llama-ext memory breakdown with fallback estimates; respect explicit settings and leave multi-device expansion outside initial scope.

**Local sequence:** Follow O3 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02, X12.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.
- **X12: llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables.** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

**Decision gate:** [DEC-X04](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x04)

**Verification / completion:** np1/np3+ mixed workloads preserve correct outputs; incompatible state is rejected; memory ceilings/eviction/cancel are deterministic; prefix reuse and fit savings are measured on targeted model families.

**Traceability:** original audit `D020`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/sampling_grammar_fit_helpers.md).

<a id="oe-d031"></a>

#### OE-D031 — Unified KV mode and admission

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add an opt-in unified KV cache so -np does not rigidly split context; define free-cell admission, spill/retry/preemption and fairness rather than relying on allocations succeeding.

**Local sequence:** Establish sequence-state correctness and bounded admission in O3 before any preemption/priority research.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** np1/np3+ mixed workloads preserve correct outputs; incompatible state is rejected; memory ceilings/eviction/cancel are deterministic; prefix reuse and fit savings are measured on targeted model families.

**Traceability:** original audit `D031`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/kv_cache_state.md).

### O4 work items

<a id="oe-d032"></a>

#### OE-D032 — Reasoning field and effort compatibility

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Support the chosen reasoning/reasoning_content convention and reasoning_effort including none with explicit supported-family behavior and streaming parity.

**Local sequence:** Follow O4 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** [DEC-X03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x03)

**Verification / completion:** Official Python/JS clients parse all supported modes; refusal is explicit; streamed lifecycle/usage/tools/reasoning/cancel agree with non-streaming output; JX adapter acceptance is an integration gate, not a blocker to direct clients.

**Traceability:** original audit `D032`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/api_standards.md).

<a id="oe-d033"></a>

#### OE-D033 — Strict and allowed tools

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Support allowed_tools and strict function schemas with clear unsupported cases and official-client tests.

**Local sequence:** Follow O4 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** [DEC-X03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x03)

**Verification / completion:** Official Python/JS clients parse all supported modes; refusal is explicit; streamed lifecycle/usage/tools/reasoning/cancel agree with non-streaming output; JX adapter acceptance is an integration gate, not a blocker to direct clients.

**Traceability:** original audit `D033`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d034"></a>

#### OE-D034 — Reranking endpoint

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add /v1/rerank and rank-pooling model launch/validation, scores/result semantics and documentation; coordinate JX #288.

**Local sequence:** Follow O4 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** [DEC-X03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x03)

**Verification / completion:** Official Python/JS clients parse all supported modes; refusal is explicit; streamed lifecycle/usage/tools/reasoning/cancel agree with non-streaming output; JX adapter acceptance is an integration gate, not a blocker to direct clients.

**Traceability:** original audit `D034`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d035"></a>

#### OE-D035 — Responses API

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Implement scoped /v1/responses support and proposed input_tokens surface, including tool/reasoning items, streaming lifecycle, errors and cancellation; coordinate JX routing and explicitly decide state persistence policy.

**Local sequence:** Follow O4 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** [DEC-X03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x03)

**Verification / completion:** Official Python/JS clients parse all supported modes; refusal is explicit; streamed lifecycle/usage/tools/reasoning/cancel agree with non-streaming output; JX adapter acceptance is an integration gate, not a blocker to direct clients.

**Traceability:** original audit `D035`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/api_standards.md).

<a id="oe-d036"></a>

#### OE-D036 — Anthropic Messages API

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Implement /v1/messages and count_tokens, x-api-key/auth rules, SSE events, tools and thinking-budget mapping; coordinate JX #302 and refusal cases.

**Local sequence:** Follow O4 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** [DEC-X03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x03)

**Verification / completion:** Official Python/JS clients parse all supported modes; refusal is explicit; streamed lifecycle/usage/tools/reasoning/cancel agree with non-streaming output; JX adapter acceptance is an integration gate, not a blocker to direct clients.

**Traceability:** original audit `D036`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/api_standards.md).

### O5 work items

<a id="oe-d021"></a>

#### OE-D021 — Retain chat seam or fully own chat layer

**State:** Owner decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Choose low-maintenance upstream common chat behind a seam versus full ownership, accepting loss of automatic new-template coverage and recurring parser maintenance.

**Local sequence:** Follow O5 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O01](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o01)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Approximately 20 representative templates pass tools/reasoning/grammar stream and non-stream parity; unsupported families are disclosed; new upstream templates have an assigned maintenance path.

**Traceability:** original audit `D021`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d022"></a>

#### OE-D022 — Jinja and JSON-schema engines

**State:** Conditional on ownership choice. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Copy/vendor the upstream Jinja engine and schema converter, own render glue, format detection, parser overrides and lazy grammar; preserve licensing and update provenance.

**Local sequence:** Follow O5 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O01](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o01)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Approximately 20 representative templates pass tools/reasoning/grammar stream and non-stream parity; unsupported families are disclosed; new upstream templates have an assigned maintenance path.

**Traceability:** original audit `D022`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d023"></a>

#### OE-D023 — Family-specific tool/reasoning parsers

**State:** Conditional on ownership choice. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Implement and test streaming/non-streaming tool and reasoning parsers for the report's supported model families, generic fallback warnings and explicit compatibility limits.

**Local sequence:** Follow O5 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O01](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o01)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Approximately 20 representative templates pass tools/reasoning/grammar stream and non-stream parity; unsupported families are disclosed; new upstream templates have an assigned maintenance path.

**Traceability:** original audit `D023`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/chat_templates_tools.md).

<a id="oe-d024"></a>

#### OE-D024 — Golden templates and common-free build

**State:** Conditional on ownership choice. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Build a roughly 20-template golden corpus with complete/streamed parity, tools/reasoning/grammar tests; only then set LLAMA_BUILD_COMMON=OFF.

**Local sequence:** Complete D022/D023 golden parity before disabling common linkage.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O01](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o01)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Approximately 20 representative templates pass tools/reasoning/grammar stream and non-stream parity; unsupported families are disclosed; new upstream templates have an assigned maintenance path.

**Traceability:** original audit `D024`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

### O6 work items

<a id="oe-d025"></a>

#### OE-D025 — Speculative verifier

**State:** Planned / design decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Implement k+1 target-row verification, accepted-prefix commit, rejected-tail rollback, sampler state, probability/logprob and stop handling; decide owned verifier vs common_speculative reuse.

**Local sequence:** Complete O2 owned sampler state/rollback contracts before integrating verifier rows.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O02](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o02)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Exact correctness/rollback tests pass under multi-slot cancellation; memory/acceptance/latency are measured; low-acceptance modes disable safely; no unconditional speedup/default-on claim.

**Traceability:** original audit `D025`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d026"></a>

#### OE-D026 — Adaptive n-gram drafting

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add per-slot n-gram proposals, acceptance EMA, bounded scratch state and kill switch; prove measured benefit and correctness before default changes.

**Local sequence:** Complete D025 verifier and baseline measurements before adaptive proposals.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O02](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o02)

**Verification / completion:** Exact correctness/rollback tests pass under multi-slot cancellation; memory/acceptance/latency are measured; low-acceptance modes disable safely; no unconditional speedup/default-on claim.

**Traceability:** original audit `D026`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md).

<a id="oe-d027"></a>

#### OE-D027 — Recurrent rollback

**State:** Planned research. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Use supported n_rs_seq snapshots correctly, bound rollback windows and verify recurrent/hybrid correctness and multi-slot scheduling.

**Local sequence:** Follow O6 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O02](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o02)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Exact correctness/rollback tests pass under multi-slot cancellation; memory/acceptance/latency are measured; low-acceptance modes disable safely; no unconditional speedup/default-on claim.

**Traceability:** original audit `D027`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/speculative_decoding.md).

<a id="oe-d028"></a>

#### OE-D028 — Second draft model

**State:** Later / opt-in. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add separate draft context with vocabulary/compatibility checks, memory accounting and speed probe; keep accepted draft KV and trim rejected tails; decline configurations that regress throughput.

**Local sequence:** Complete verifier + n-gram baseline; add vocabulary compatibility, second-context memory and speed probe.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O02](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o02)

**Verification / completion:** Exact correctness/rollback tests pass under multi-slot cancellation; memory/acceptance/latency are measured; low-acceptance modes disable safely; no unconditional speedup/default-on claim.

**Traceability:** original audit `D028`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Replacing%20llama%20cpp%20common%20code.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/speculative_decoding.md).

<a id="oe-d029"></a>

#### OE-D029 — Suffix-based drafting

**State:** Optional research. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate cross-request suffix-automaton drafting with bounded memory, privacy/isolation and measured acceptance; do not assume it is an approved default.

**Local sequence:** Follow O6 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O02](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o02)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Exact correctness/rollback tests pass under multi-slot cancellation; memory/acceptance/latency are measured; low-acceptance modes disable safely; no unconditional speedup/default-on claim.

**Traceability:** original audit `D029`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/speculative_decoding.md).

<a id="oe-d030"></a>

#### OE-D030 — MTP, EAGLE3 and DFlash

**State:** Upstream dependency / decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Wait for public llama.h support, or explicitly choose an experimental single-slot driver. Multi-sequence support/rollback and private API churn are unresolved constraints.

**Local sequence:** Follow O6 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X12.

- **X12: llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables.** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

**Decision gate:** [DEC-O02](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o02)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Exact correctness/rollback tests pass under multi-slot cancellation; memory/acceptance/latency are measured; low-acceptance modes disable safely; no unconditional speedup/default-on claim.

**Traceability:** original audit `D030`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Replacing%20llama%20cpp%20common%20code/speculative_decoding.md).

### O7 work items

<a id="oe-d012"></a>

#### OE-D012 — Shared release convention

**State:** Planned documentation. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Write docs/release-convention.md covering stable tags, asset names/archive layouts, checksums, executable version contract and compatibility for JX installer/audio apps before Song Deck M3.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D012`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d014"></a>

#### OE-D014 — Scheduled vendor pin-update pipeline

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Monthly/scheduled action to update llama.cpp/ggml vendor specs, open a PR with common header changes and golden-output comparisons; keep httplib advisories and pin provenance visible.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X08, X12.

- **X08: Onyx Engine ↔ BackTrack.** Onyx pin upgrade can be reviewed independently; claiming cross-repo pin alignment waits for BackTrack change and validation. Current BackTrack state must be inspected then.
- **X12: llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables.** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D014`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Onyx%20Engine%20improvement%20opportunities.md).

<a id="oe-d015"></a>

#### OE-D015 — BackTrack ggml pin alignment

**State:** Cross-repo dependency. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Check BackTrack with scripts/check-ggml-pin.sh and align its ggml revision with Onyx f1aa6a3 after bumps. The other repo's current state was not audited here.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X08, X12.

- **X08: Onyx Engine ↔ BackTrack.** Onyx pin upgrade can be reviewed independently; claiming cross-repo pin alignment waits for BackTrack change and validation. Current BackTrack state must be inspected then.
- **X12: llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables.** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D015`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d049"></a>

#### OE-D049 — Binary build attestations and SBOM

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add/verify provenance for release binaries and an SBOM, and choose immutable releases. npm publish --provenance already exists and is not equivalent to binary artifact attestation.

**Local sequence:** Attest the final signed archive bytes, not intermediate unsigned binaries; existing npm provenance is a different scope.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** [DEC-X10](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x10)

**Verification / completion:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D049`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d050"></a>

#### OE-D050 — Platform signing and notarization

**State:** External dependency / decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Decide budget/identities for Windows Authenticode and Apple notarization, then wire and verify native signing; coordinate the corresponding JX installer trust chain.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** [DEC-X05](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x05)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D050`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d051"></a>

#### OE-D051 — Reproducible build evidence

**State:** Research / verification. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Document toolchain and source provenance, reproduce artifacts where practical and investigate nondeterminism; signed/checksummed files alone do not prove reproducibility.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D051`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d052"></a>

#### OE-D052 — CUDA and ROCm release archives

**State:** Decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Choose supported toolkit/GPU targets and package dependency-complete CUDA or ROCm variants; validate hardware startup and checksums.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** [DEC-X08](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x08)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D052`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d053"></a>

#### OE-D053 — Metal in Apple arm64 archive

**State:** Decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Enable and verify Metal acceleration in the published darwin-arm64 archive; an existing CPU-capable archive is not completion.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** [DEC-X08](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x08)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D053`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d054"></a>

#### OE-D054 — Dynamic backend/CPU dispatch

**State:** Deferred candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate GGML_BACKEND_DL runtime backend/CPU dispatch instead of separate variants, with portable fallback and trusted library loading.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09, X12.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.
- **X12: llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables.** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

**Decision gate:** [DEC-X08](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x08)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D054`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d055"></a>

#### OE-D055 — arm64 and CUDA container images

**State:** Decision / candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Extend published CPU Docker image to arm64 and/or CUDA with model mount/startup documentation and real accelerator verification.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** [DEC-X08](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x08), [DEC-X09](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x09)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D055`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/independent-tool-plan.md).

<a id="oe-d056"></a>

#### OE-D056 — Windows arm64 build

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate Windows arm64 toolchain/runtime coverage if demand warrants; separate from already shipped Linux arm64.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** [DEC-X08](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x08)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D056`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d057"></a>

#### OE-D057 — Homebrew and winget

**State:** Deferred. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add package-manager distribution after releases and signing policy stabilize; retain checksum/version/update contract.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-X09](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x09)

**Verification / completion:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D057`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/independent-tool-plan.md).

<a id="oe-d058"></a>

#### OE-D058 — Llama 3 RoPE and tokenizer fixes

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Support Llama 3 RoPE and llama-bpe pre-tokenizer in the numpy converter with fixture and real-model validation.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o03)

**Verification / completion:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D058`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d059"></a>

#### OE-D059 — Chat-template metadata and gguf-py rebase

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Embed the model chat template, rebase onto vendored gguf-py and evaluate Q8_0 output without claiming broad architecture support.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o03)

**Verification / completion:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D059`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Onyx%20Engine%20improvement%20opportunities.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/models_quant_conversion.md).

<a id="oe-d060"></a>

#### OE-D060 — Conversion fallback and architecture scope

**State:** Owner decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Choose numpy-only fixes vs optional upstream torch converter fallback; add Qwen/Gemma/Mistral only with demand and architecture fixtures.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o03)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D060`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/models_quant_conversion.md).

<a id="oe-d061"></a>

#### OE-D061 — Hugging Face GGUF resolver and projector pairing

**State:** Owner decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Choose whether -hf repo[:quant] resolution and mmproj auto-pairing belong in Onyx or JX; preserve no request-triggered arbitrary I/O and pinned provenance.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o03)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D061`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/models_quant_conversion.md).

<a id="oe-d062"></a>

#### OE-D062 — Packed NVFP4/MXFP4 source checkpoints

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Research architecture-specific U8 packed tensors/scales and quantization provenance; current converter refusal is intentional, not generic support already present.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o03)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D062`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/models_quant_conversion.md).

<a id="oe-d068"></a>

#### OE-D068 — Strict signed release inventory

**State:** Optional security tier. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Optional strict Ed25519 inventory requires reviewed per-app public key, matching environment secret and signed dry-run validation; current standard inventory profile does not claim this.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** [DEC-X10](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x10)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D068`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/.release/README.md).

<a id="oe-d069"></a>

#### OE-D069 — Release environment and trust verification

**State:** Verification / administration. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Record actual environment reviewers/restrictions, required checks, signing/registry identities and native package/install/update smoke results. Published releases do not settle protection settings inaccessible to this connector.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D069`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/.release/README.md).

<a id="oe-d080"></a>

#### OE-D080 — Action SHA pins and runner hardening

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Review remaining tag-pinned Actions, permissions and OIDC/cache boundaries; consider harden-runner/Scorecard. Some release steps are already SHA-pinned; target only remaining gaps and coordinate open dependency PRs.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `D080`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-b-vendor"></a>

#### OE-B-Vendor — Publish reusable vendor packages

**State:** Optional / conditional. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** The branch Song Deck plan allows publishing reusable vendor packages if useful; reusable scripts already shipped. Confirm package publication/distribution need and current registry state before creating new work.

**Audit disposition:** Structured-output, release convention and converter work from this branch are consolidated into the main document rows above.

**Local sequence:** Follow O7 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X09.

- **X09: Onyx Engine + shared release infrastructure → JX Runtime / audio apps.** Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers.

**Decision gate:** [DEC-X09](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x09)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published.

**Traceability:** original audit `B-Vendor`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/b44a103c0289f0495e567797c52deed07b1fd58c/docs/onyx-engine.md).

### O8 work items

<a id="oe-i71"></a>

#### OE-I71 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-003\] Manually verify OpenAI-compatible response formatting

**State:** Verification. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Verify OpenAI response shape, SSE, usage and finish_reason against real clients. Overlaps the independent-tool conformance plan.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `#71`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/71).

<a id="oe-i70"></a>

#### OE-I70 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-002\] Manually review shared-state synchronization

**State:** QA review / not confirmed defect. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Review mutex/shared-state ownership and shutdown/cancellation paths; add evidence-based concurrency tests for real risks.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `#70`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/issues/70).

<a id="oe-d039"></a>

#### OE-D039 — Latency, cache and finish metrics

**State:** Planned / research extensions. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Extend current metrics with TTFT, TPOT/end-to-end/queue histograms and finish/cache counters; bound cardinality and align names/units. X-Request-Id/basic counters already shipped.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D039`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Onyx%20Engine%20improvement%20opportunities.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/serving_techniques.md).

<a id="oe-d040"></a>

#### OE-D040 — Structured access logging and tracing

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Consider content-free JSON access logs and optional traceparent correlation; retain bounded fields and avoid prompt or credential logging.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D040`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d042"></a>

#### OE-D042 — Official-client conformance

**State:** Planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Run official OpenAI Python and JavaScript clients against every supported route; add Responses compliance once implemented; publish rerunnable results. Overlaps #71.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D042`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/independent-tool-plan.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/Onyx%20Engine%20improvement%20opportunities.md).

<a id="oe-d043"></a>

#### OE-D043 — Same-build benchmark suite

**State:** Hardware / real models. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Publish commands/methodology and matched llama-server comparisons for TTFT, tokens/s at np1/2/4, RSS and prefix reuse; separate synthetic wiring tests from real model behavior.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-X11](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x11)

**Verification / completion:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D043`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/independent-tool-plan.md).

<a id="oe-d044"></a>

#### OE-D044 — Nightly real-model tests

**State:** Planned CI. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Run real model behavior at least nightly, including tools, multimodal, schema and concurrency cases instead of relying entirely on tiny generated smoke models.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-X11](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x11)

**Verification / completion:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D044`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/independent-tool-plan.md).

<a id="oe-d045"></a>

#### OE-D045 — ASan and UBSan smoke

**State:** Planned CI. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add sanitizer builds/smoke coverage for request processing, decode, cancellation and cleanup.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D045`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/reports/independent-tool-plan.md).

<a id="oe-d046"></a>

#### OE-D046 — Fuzz request and conversion boundaries

**State:** Planned CI. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add libFuzzer harnesses for request JSON, data URIs and safetensors/converter metadata; keep seeded corpora and sanitizer coverage.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D046`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d047"></a>

#### OE-D047 — Assert RPC is off

**State:** Planned CI. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add an explicit CI/build assertion that RPC is not accidentally enabled in released binaries.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D047`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d048"></a>

#### OE-D048 — Static analysis expansion

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate clang-tidy/cppcheck/CodeQL and address actionable findings with a maintained baseline.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D048`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d078"></a>

#### OE-D078 — ThreadSanitizer concurrency job

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Add a separate TSan job with np4 concurrent streams and vetted suppressions for vendor noise; ASan/UBSan alone does not prove synchronization correctness.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D078`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md).

<a id="oe-d079"></a>

#### OE-D079 — Expanded fuzz scope and Python property tests

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Consider a pinned-tree GGUF-load fuzz harness and Python atheris/Hypothesis tests for safetensors header length, offsets and shape-by-dtype size; use short PR runs and longer nightly corpora.

**Local sequence:** Follow O8 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** No new owner choice identified; preserve the stated scope/defaults. A conditional research item still needs explicit promotion.

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures.

**Traceability:** original audit `D079`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/engineering_security.md); [source 2](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/models_quant_conversion.md).

### O9 work items

<a id="oe-o1"></a>

#### OE-O1 — Nsight profiling workflow

**State:** Branch-only planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Nsight Systems then Compute recipes, synthetic workloads, report digest and toolchain/GPU/settings provenance; measure tracing overhead and justify optimizations from baseline evidence.

**Audit disposition:** Document calls this accepted direction, but it is not merged into main. Shared Studio contracts C1/C2/C3/C4/C6 are cross-repo dependencies; this audit does not verify Studio implementation.

**Local sequence:** Follow O9 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-X06](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x06)

**Verification / completion:** Tracing overhead and privacy are bounded; old JX ignores unknown props safely; GPU build matrix and matched mixed workloads show correctness and measured changes.

**Traceability:** original audit `O1`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/91d9294170d98d2464bc8c00db802b76b8f19bf3/docs/nvidia-developer-workflows-plan.md).

<a id="oe-o2"></a>

#### OE-O2 — Optional NVTX and safe profiling properties

**State:** Branch-only planned / conditional. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Compile-time optional bounded NVTX ranges for admission/prefill/decode/cache/media/cancel, documented units/retention/overhead and additive safe props with old-build fallback.

**Audit disposition:** Document calls this accepted direction, but it is not merged into main. Shared Studio contracts C1/C2/C3/C4/C6 are cross-repo dependencies; this audit does not verify Studio implementation.

**Local sequence:** Follow O9 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X10.

- **X10: AI Server Studio ↔ JX Runtime ↔ Onyx Engine.** Developer measurement recipes can proceed independently. Profile/setup UI integration waits for agreed schemas and Studio consumption; Onyx optional properties must not break old JX.

**Decision gate:** [DEC-X07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x07), [DEC-X06](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x06)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Tracing overhead and privacy are bounded; old JX ignores unknown props safely; GPU build matrix and matched mixed workloads show correctness and measured changes.

**Traceability:** original audit `O2`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/91d9294170d98d2464bc8c00db802b76b8f19bf3/docs/nvidia-developer-workflows-plan.md).

<a id="oe-o3"></a>

#### OE-O3 — NVIDIA build presets

**State:** Branch-only planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evidence-backed CUDA/compiler/ggml presets, compute capability prerequisites, portable fallback and verified archive/checksum contract for JX.

**Audit disposition:** Document calls this accepted direction, but it is not merged into main. Shared Studio contracts C1/C2/C3/C4/C6 are cross-repo dependencies; this audit does not verify Studio implementation.

**Local sequence:** Follow O9 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X10.

- **X10: AI Server Studio ↔ JX Runtime ↔ Onyx Engine.** Developer measurement recipes can proceed independently. Profile/setup UI integration waits for agreed schemas and Studio consumption; Onyx optional properties must not break old JX.

**Decision gate:** [DEC-X06](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x06)

**Verification / completion:** Tracing overhead and privacy are bounded; old JX ignores unknown props safely; GPU build matrix and matched mixed workloads show correctness and measured changes.

**Traceability:** original audit `O3`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/91d9294170d98d2464bc8c00db802b76b8f19bf3/docs/nvidia-developer-workflows-plan.md).

<a id="oe-o4"></a>

#### OE-O4 — Workload-aware scheduling

**State:** Branch-only planned. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Measure mixed text/media latency and cancellation; implement bounded prefill, fair admission and safe media staging only within engine ownership rules, then compare matched p50/p95/throughput.

**Audit disposition:** Document calls this accepted direction, but it is not merged into main. Shared Studio contracts C1/C2/C3/C4/C6 are cross-repo dependencies; this audit does not verify Studio implementation.

**Local sequence:** Follow O9 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X10.

- **X10: AI Server Studio ↔ JX Runtime ↔ Onyx Engine.** Developer measurement recipes can proceed independently. Profile/setup UI integration waits for agreed schemas and Studio consumption; Onyx optional properties must not break old JX.

**Decision gate:** [DEC-X06](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-x06)

**Verification / completion:** Tracing overhead and privacy are bounded; old JX ignores unknown props safely; GPU build matrix and matched mixed workloads show correctness and measured changes.

**Traceability:** original audit `O4`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/91d9294170d98d2464bc8c00db802b76b8f19bf3/docs/nvidia-developer-workflows-plan.md).

### O10 work items

<a id="oe-d037"></a>

#### OE-D037 — Model detail and health aliases

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate /v1/models/{id}, stable created metadata and any additional health alias behavior for clients. Existing /health readiness and 503-loading behavior are already implemented.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D037`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/api_standards.md).

<a id="oe-d038"></a>

#### OE-D038 — Fill-in-the-middle code completion

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Consider /infill or completions suffix support for editor clients, with model/template prerequisites and tests.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D038`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/api_standards.md).

<a id="oe-d063"></a>

#### OE-D063 — Idle sleep for other supervisors

**State:** Optional candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Consider --sleep-idle-seconds for supervisors without idle unloading; JX already stops idle engine processes after its configured timeout.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X02.

- **X02: JX Runtime → Onyx Engine integration.** Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests.

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D063`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d064"></a>

#### OE-D064 — Cache-aware fairness and media prefill

**State:** Research / later. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** After cache work, measure cache-aware scheduling, fairness and long image/audio prefill stalls; design bounded scheduling without violating single-owner decode state.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D064`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/serving_techniques.md).

<a id="oe-d065"></a>

#### OE-D065 — llguidance

**State:** Conflicting proposals / decision. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Resolve R1's deferred optional llguidance build versus R2's rejection of the Rust toolchain; do not count as an accepted feature until a direction is chosen.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07), [DEC-O06](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o06)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D065`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/ROADMAP.md).

<a id="oe-d066"></a>

#### OE-D066 — llama_batch_ext and llama_process migration

**State:** Future API research. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Assess moving off the legacy batch API as upstream evolves; owned batch helper migration is already done, full new API adoption is a distinct future candidate.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X12.

- **X12: llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables.** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D066`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/upstream_llamacpp.md).

<a id="oe-d070"></a>

#### OE-D070 — Audio transcription API

**State:** Low-priority research. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Consider /v1/audio/transcriptions only with an ASR-capable model and multipart parsing; distinguish this optional research from the explicit TTS/realtime non-goals.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D070`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/api_standards.md).

<a id="oe-d071"></a>

#### OE-D071 — Additional native completion controls

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate n_probs/post_sampling_probs aliases, return_tokens, t_max_predict_ms and explicit id_slot selection, plus GET /slots; define supported semantics before adding compatibility promises.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D071`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/upstream_llamacpp.md).

<a id="oe-d072"></a>

#### OE-D072 — LoRA adapter hot swap

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate llama_adapter_lora_init/set-adapters APIs at the actual pin, safe slot/model ownership, cache invalidation and failure handling before exposing hot swapping.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D072`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/upstream_llamacpp.md).

<a id="oe-d073"></a>

#### OE-D073 — Backend/GPU sampler chains

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate llama_context_params.samplers to avoid full-vocabulary logits copies for compatible greedy/top-k requests; preserve full-vocabulary logprobs through a per-request fallback.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X12.

- **X12: llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables.** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D073`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/upstream_llamacpp.md).

<a id="oe-d074"></a>

#### OE-D074 — Additional upstream load and memory flags

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate pin-supported --moe-cache-mib, --n-cpu-ffn, tensor split mode, --load-mode, --lazy-mode, --repack, --swa-full and fit-target/fit-context controls. Verify APIs at the actual pin; upstream availability is not proof Onyx can pass a flag unchanged.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** X12.

- **X12: llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables.** Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract.

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D074`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/upstream_llamacpp.md).

<a id="oe-d075"></a>

#### OE-D075 — Encoded media cache and prefill batching

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate bounded mtmd batch token budgets and cache encoded image embeddings by image hash to avoid repeated encoder cost; preserve media/model identity and single-thread ownership.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D075`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/upstream_llamacpp.md).

<a id="oe-d076"></a>

#### OE-D076 — Video and expanded audio input forms

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate video/video_url content parts and expanded audio data-URI inputs beyond the current supported image/audio surface; define exact supported forms and regressions before claiming completion.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D076`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/upstream_llamacpp.md).

<a id="oe-d081"></a>

#### OE-D081 — K-quant and i-quant production

**State:** Low-priority research. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** If promoted, call the vendored llama_model_quantize C API with optional user imatrix rather than reimplementing quantization. Distinct from Q8_0 output and loading existing quantized models.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07), [DEC-O03](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o03)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D081`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/models_quant_conversion.md).

<a id="oe-d083"></a>

#### OE-D083 — Model hashes and optional signatures

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Record GGUF/source-safetensors SHA-256 in safe props/models metadata; optionally verify OpenSSF model signatures and bind any HF resolver to verified digests.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D083`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/models_quant_conversion.md).

<a id="oe-d084"></a>

#### OE-D084 — Request priorities with starvation bounds

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Consider interactive-vs-batch request priority and bounded cache-hit queue reordering; preemption requires unified-KV/cache work and explicit fairness tests.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D084`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/serving_techniques.md).

<a id="oe-d085"></a>

#### OE-D085 — Shared system-prompt reuse and disk tier

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate copying a common prefix with llama_memory_seq_cp and an opt-in disk prompt cache surviving restarts; measure disk restore vs recompute and guard state/model compatibility. Overlaps planned slot save/restore.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D085`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/serving_techniques.md).

<a id="oe-d086"></a>

#### OE-D086 — Compiled grammar cache and mask scheduling

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate bounded schema-keyed compiled grammar caching and parallel/overlapped grammar masks to prevent one slow grammar stalling all slots; preserve thread ownership. Separate from choosing llguidance.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07), [DEC-O06](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o06)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D086`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/serving_techniques.md).

<a id="oe-d087"></a>

#### OE-D087 — Graceful drain and warmup

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Evaluate a bounded SIGTERM drain (stop admissions, finish active slots, then exit) and startup empty-decode warmup to remove GPU-JIT first-request spikes; verify actual current lifecycle behavior before changes.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D087`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/serving_techniques.md).

<a id="oe-d088"></a>

#### OE-D088 — Additional reasoning and tool conventions

**State:** Research candidate. **Owner:** Onyx-Engine implementation maintainer; no individual assignment is implied.

**Work:** Consider reasoning.exclude, round-tripping interleaved reasoning input, template-derived parallel_tool_calls defaults and consistent call IDs; current tool parsing exists, so extend only missing conventions.

**Local sequence:** Follow O10 entry/exit gates above. Split this item into reviewable behavior changes; do not merge the whole package at once.

**Other-repo dependency:** None required for the repository-local change. Any hardware or owner-decision gate still applies..

**Decision gate:** [DEC-O07](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md#dec-o07)

**Verification / completion:** Record the accept/defer choice, exact supported scope and a reproducible experiment for this item. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent.

**Traceability:** original audit `D088`; [source 1](https://github.com/JX-Holdings-LLC/Onyx-Engine/blob/32ec168b3a309fb146783e6c09896563b1412590/research_notes/Onyx%20Engine%20improvement%20opportunities/api_standards.md).

## Reconciliation and overlap register

- Closed **#66** as not applicable: current catch(error) already throws error. It is a stale source finding, not a newly fixed runtime bug.
- **#68** remains open for its current vendor-spec parser suggestion; the old title alleging a missing readSpec export is obsolete.
- **#64** requires QA artifact/environment triage; published engine binaries already exist.
- **#65/#70/#71** overlap concurrency/API conformance review and O8. Reuse evidence rather than creating three unsupported defects.
- Independent-tool plan items **1–8**, batch helpers, b11820 pin, text systemone, basic metrics/queue budget/readiness, base64 embeddings, baseline/Vulkan/Linux-arm64 archives and CPU Docker already shipped; only residual work is scheduled.
- Published **v0.6.2** supersedes first-release tasks. Empty v0.4.1/v0.5.1 drafts still need deliberate cleanup; native QA/signing/protections remain distinct.
- JX #327 is closed; no JX link migration blocks old-stub retirement. Check other consumers before removing any remaining stub.

## Validation and release discipline

Run CMake build and `ctest --test-dir build --output-on-failure`, then `bash scripts/smoke-test.sh` and converter checks when touched; native Windows/macOS and real GPU/model behavior need their own evidence. Documentation-only changes require link/version consistency checks; no engine behavior is claimed from those. Every-main-change version policy still applies, including docs; vendor app versions move only when affected.

No dates or effort estimates in historical research are adopted as commitments. No feature here authorizes agent/RAG/model-selection ownership in JX, or a router, built-in tools/MCP, web UI, TTS or realtime surface in Onyx. Upstream TODO quotations are evidence, not automatically local tickets.
