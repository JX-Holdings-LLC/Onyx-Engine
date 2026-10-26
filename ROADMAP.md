# Onyx Engine remaining-work roadmap

This roadmap consolidates the unfinished architecture plans, research, repository TODOs, open issues and unmerged proposals reviewed on 2026-10-26. It defines proposed delivery order and dependencies; it does not approve conditional features or change the product boundary.

- [Detailed implementation plan](reports/consolidated-implementation-plan.md) — every remaining audit item, local sequence, verification and cross-repo wait.
- [Owner decisions](https://github.com/jxburros/JX-Runtime/blob/main/development-docs/owner-decisions.md) — options, recommendations and what remains blocked. The combined register lives in JX Runtime; maintainers need access to that repository. Merge the companion JX planning PR first to make this permanent link available.
- [Prior roadmap snapshot](reports/archive/roadmap-before-consolidation-2026-10-26.md) — history and original source references.

## Current baseline

Onyx Engine 0.6.2 is published with nine platform archives and checksums. The engine pins llama.cpp b11820 and matching ggml f1aa6a3. Independent-tool items 1–8 are shipped; item 9 evidence work remains. Native decision text, base64 embeddings, basic metrics/admission/readiness and CPU Docker already exist.

After evidence-based closure, the tracker contains 12 open issues and four dependency PRs. The implementation plan retains 109 audit entries because document work and research overlap issues; that is not a count of independent features.

## Delivery sequence

| Package | Outcome | Dependency / exit gate |
|---|---|---|
| O0: Reconcile QA, documentation and dependency PRs | Now | Current main plus issue bodies and release evidence. Every QA finding is reproduced or explicitly dismissed; parser/build errors retain useful context; doc/link/version checks pass and duplicate review requests share one evidence record. |
| O1: Small API, hardening and operator controls | First implementation tranche | No cross-repo blocker to direct-engine work; resolve timeout/schema/optional label policy. Unit/CLI/HTTP smoke covers range/type/precedence/refusal cases; per-slot context and normalized embeddings are correct; timeouts do not truncate valid SSE; media decision tests evaluate text/media in the same batch. |
| O2: Common Stage 1 and owned sampler seam | After O1 parsing baseline; before speculation/full chat ownership | Golden baseline at exact pin; do not redo already shipped batch helpers. Golden token IDs/logprobs/grammar/seed behavior and streaming outputs match the baseline; error/depth/cancellation limits hold; common dependency changes are measured, not inferred. |
| O3: Common Stage 2: cache, checkpoints and fit | After O2 capability seams | Verified llama state APIs at pin; owner decision on unstable fit fallback; hardware/model evidence. np1/np3+ mixed workloads preserve correct outputs; incompatible state is rejected; memory ceilings/eviction/cancel are deterministic; prefix reuse and fit savings are measured on targeted model families. |
| O4: Client protocols and model metadata | After O1; may overlap O2/O3 | Backend parsing contracts and DEC-X03 protocol boundaries. Official Python/JS clients parse all supported modes; refusal is explicit; streamed lifecycle/usage/tools/reasoning/cancel agree with non-streaming output; JX adapter acceptance is an integration gate, not a blocker to direct clients. |
| O5: Common Stage 3: chat-layer ownership | Conditional; after O2 and explicit ownership decision | DEC-O01; budget to maintain family templates/parsers and golden corpus. Approximately 20 representative templates pass tools/reasoning/grammar stream and non-stream parity; unsupported families are disclosed; new upstream templates have an assigned maintenance path. |
| O6: Common Stage 4: speculative generation | After O2 sampler; independent of full O5 ownership | DEC-O02, memory reservations and rollback semantics; hardware benchmark baseline. Exact correctness/rollback tests pass under multi-slot cancellation; memory/acceptance/latency are measured; low-acceptance modes disable safely; no unconditional speedup/default-on claim. |
| O7: Release, converter and distribution maintenance | Parallel after small-contract baseline | Distribution/converter/signing decisions and platform resources. Pin/header/golden diffs reviewed; hashes/layout/version and native smoke match contract; BackTrack pins checked independently; converter fixtures and real models validate supported formats; no claim that optional packages are already published. |
| O8: Conformance, robustness and performance evidence | Build early; rerun after each affected phase | Representative hardware/models and CI capacity. Rerunnable reports retain build/model/config provenance, TTFT/tokens-per-second/RSS/cache distributions and regression thresholds; false QA findings are not counted as engine failures. |
| O9: NVIDIA profiling and measured scheduling | Separate opt-in workstream | DEC-X06 plus bounded profiling contract; baseline O8 and engine ownership. Tracing overhead and privacy are bounded; old JX ignores unknown props safely; GPU build matrix and matched mixed workloads show correctness and measured changes. |
| O10: Research candidates awaiting promotion | Unscheduled | Decision or evidence trigger; no feature is accepted merely by being listed. Decision, evidence, supported scope and rollback are recorded; unchanged defaults remain the safe outcome when evidence is absent. |

## Work covered by each package

### O0 — Reconcile QA, documentation and dependency PRs

Triage source findings against full code; fix substantiated parser/diagnostic issues; close false reports with evidence; update stale release/issue tables and merge dependency bumps through version validation.

- [OE-PR77 — build(deps): Bump actions/checkout from 4 to 7](reports/consolidated-implementation-plan.md#oe-pr77) — Open PR.
- [OE-PR76 — build(deps): Bump hendrikmuhs/ccache-action from 1.2.20 to 1.2.24](reports/consolidated-implementation-plan.md#oe-pr76) — Open PR.
- [OE-PR75 — build(deps): Bump docker/setup-buildx-action from 3 to 4](reports/consolidated-implementation-plan.md#oe-pr75) — Open PR.
- [OE-PR74 — build(deps): Bump docker/build-push-action from 6 to 7](reports/consolidated-implementation-plan.md#oe-pr74) — Open PR.
- [OE-I72 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-3\] `scripts/tools/qa-check.js`: This is reasonable, but when it fails the script currently has to infer the reason elsewhe...](reports/consolidated-implementation-plan.md#oe-i72) — Open / suggestion.
- [OE-I69 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-3\] `README.md`: This can make the introduction harder to scan and maintain, especially when implementation...](reports/consolidated-implementation-plan.md#oe-i69) — Open / documentation.
- [OE-I68 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-2\] `scripts/install-vendor.js`: The two modules appear intended to share this function, but the excerpt does not show an e...](reports/consolidated-implementation-plan.md#oe-i68) — Open / changed issue scope.
- [OE-I67 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-1\] `scripts/build.js`: If `ONYX_BUILD_JOBS` is set to a non-numeric string, it silently falls back to CPU count i...](reports/consolidated-implementation-plan.md#oe-i67) — Open / suggestion.
- [OE-I65 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-001\] Review AI-flagged unverified risks from code review](reports/consolidated-implementation-plan.md#oe-i65) — QA review / umbrella.
- [OE-I64 — \[QA\]\[Onyx Engine\]\[F-001\] binary_version failed](reports/consolidated-implementation-plan.md#oe-i64) — QA triage / not confirmed defect.
- [OE-D013 — Old empty drafts and publication prose](reports/consolidated-implementation-plan.md#oe-d013) — Reconcile / cleanup.
- [OE-D067 — Disable irrelevant browser QA in QAI-ality](reports/consolidated-implementation-plan.md#oe-d067) — Cross-repo dependency.

### O1 — Small API, hardening and operator controls

Extract pure request parsing; add models metadata, embedding dimensions, KV/MoE controls and queue timeout; upgrade httplib and bounded HTTP settings; verify actual decision-image and strict-schema behavior.

- [OE-I62 — Accept `dimensions` on /v1/embeddings](reports/consolidated-implementation-plan.md#oe-i62) — Open.
- [OE-I61 — Unit-test the request parsing helpers (stop, max tokens, sampling)](reports/consolidated-implementation-plan.md#oe-i61) — Open.
- [OE-I60 — Add a --timeout flag for the HTTP read/write timeout](reports/consolidated-implementation-plan.md#oe-i60) — Open / policy decision.
- [OE-I59 — Report context length and modalities in /v1/models](reports/consolidated-implementation-plan.md#oe-i59) — Open.
- [OE-D001 — Label-token logit_bias](reports/consolidated-implementation-plan.md#oe-d001) — Optional decision.
- [OE-D002 — Multimodal decisions](reports/consolidated-implementation-plan.md#oe-d002) — Planned.
- [OE-D003 — KV cache type flags](reports/consolidated-implementation-plan.md#oe-d003) — Planned.
- [OE-D004 — MoE CPU expert offload](reports/consolidated-implementation-plan.md#oe-d004) — Planned.
- [OE-D005 — Queue-wait deadline](reports/consolidated-implementation-plan.md#oe-d005) — Planned.
- [OE-D006 — cpp-httplib upgrade](reports/consolidated-implementation-plan.md#oe-d006) — Planned.
- [OE-D011 — Song Deck schema regression suite](reports/consolidated-implementation-plan.md#oe-d011) — Planned verification.
- [OE-D041 — Grammar resource budgets](reports/consolidated-implementation-plan.md#oe-d041) — Research candidate.
- [OE-D077 — HTTP connection and keepalive budgets](reports/consolidated-implementation-plan.md#oe-d077) — Research candidate.
- [OE-D082 — Metadata-aware pooling defaults](reports/consolidated-implementation-plan.md#oe-d082) — Research candidate.

### O2 — Common Stage 1 and owned sampler seam

Replace token/log helpers, introduce owned sampler state and bounded ordered JSON, narrow reuse capability gates and isolate upstream chat behind a seam. Make one behavior-preserving extraction per PR.

- [OE-D007 — Own token and log helpers](reports/consolidated-implementation-plan.md#oe-d007) — Planned.
- [OE-D008 — Own sampler](reports/consolidated-implementation-plan.md#oe-d008) — Planned.
- [OE-D009 — JSON replacement and limits](reports/consolidated-implementation-plan.md#oe-d009) — Planned.
- [OE-D010 — Narrow reuse gate and capability probes](reports/consolidated-implementation-plan.md#oe-d010) — Planned.

### O3 — Common Stage 2: cache, checkpoints and fit

Implement SWA/recurrent checkpoints, byte-limited RAM LRU, validated save/restore and media-aware matching; add single-GPU/CPU fit fallback and opt-in unified KV with explicit admission/failure rules.

- [OE-D016 — Context checkpoints for SWA/hybrid reuse](reports/consolidated-implementation-plan.md#oe-d016) — Planned.
- [OE-D017 — Host-RAM prompt cache](reports/consolidated-implementation-plan.md#oe-d017) — Planned.
- [OE-D018 — Slot save and restore](reports/consolidated-implementation-plan.md#oe-d018) — Planned.
- [OE-D019 — Media-aware prefix matching](reports/consolidated-implementation-plan.md#oe-d019) — Planned.
- [OE-D020 — Single-GPU plus CPU auto-fit](reports/consolidated-implementation-plan.md#oe-d020) — Planned / unstable API.
- [OE-D031 — Unified KV mode and admission](reports/consolidated-implementation-plan.md#oe-d031) — Planned.

### O4 — Client protocols and model metadata

Add rerank, reasoning/strict-tool conventions, Messages and Responses in separate PRs. Keep optional model-detail/FIM/native controls behind candidate decisions. Coordinate JX routing only for end-to-end use.

- [OE-D032 — Reasoning field and effort compatibility](reports/consolidated-implementation-plan.md#oe-d032) — Planned.
- [OE-D033 — Strict and allowed tools](reports/consolidated-implementation-plan.md#oe-d033) — Planned.
- [OE-D034 — Reranking endpoint](reports/consolidated-implementation-plan.md#oe-d034) — Planned.
- [OE-D035 — Responses API](reports/consolidated-implementation-plan.md#oe-d035) — Planned.
- [OE-D036 — Anthropic Messages API](reports/consolidated-implementation-plan.md#oe-d036) — Planned.

### O5 — Common Stage 3: chat-layer ownership

Either retain the tested upstream chat seam, or copy the licensed engines and own parsers/fallbacks with golden fixtures. Disable LLAMA_BUILD_COMMON only after full parity.

- [OE-D021 — Retain chat seam or fully own chat layer](reports/consolidated-implementation-plan.md#oe-d021) — Owner decision.
- [OE-D022 — Jinja and JSON-schema engines](reports/consolidated-implementation-plan.md#oe-d022) — Conditional on ownership choice.
- [OE-D023 — Family-specific tool/reasoning parsers](reports/consolidated-implementation-plan.md#oe-d023) — Conditional on ownership choice.
- [OE-D024 — Golden templates and common-free build](reports/consolidated-implementation-plan.md#oe-d024) — Conditional on ownership choice.

### O6 — Common Stage 4: speculative generation

Implement verifier first, adaptive per-slot n-grams second, optional draft model third; handle accepted/rejected tails, stop/logprobs and recurrent snapshots. Keep MTP/EAGLE3/DFlash gated on API maturity/explicit experiment.

- [OE-D025 — Speculative verifier](reports/consolidated-implementation-plan.md#oe-d025) — Planned / design decision.
- [OE-D026 — Adaptive n-gram drafting](reports/consolidated-implementation-plan.md#oe-d026) — Planned.
- [OE-D027 — Recurrent rollback](reports/consolidated-implementation-plan.md#oe-d027) — Planned research.
- [OE-D028 — Second draft model](reports/consolidated-implementation-plan.md#oe-d028) — Later / opt-in.
- [OE-D029 — Suffix-based drafting](reports/consolidated-implementation-plan.md#oe-d029) — Optional research.
- [OE-D030 — MTP, EAGLE3 and DFlash](reports/consolidated-implementation-plan.md#oe-d030) — Upstream dependency / decision.

### O7 — Release, converter and distribution maintenance

Automate smaller pin bumps; document release convention; expand chosen GPU builds; attest/sign/reproduce artifacts; fix converter scope with architecture fixtures and explicit HF ownership.

- [OE-D012 — Shared release convention](reports/consolidated-implementation-plan.md#oe-d012) — Planned documentation.
- [OE-D014 — Scheduled vendor pin-update pipeline](reports/consolidated-implementation-plan.md#oe-d014) — Planned.
- [OE-D015 — BackTrack ggml pin alignment](reports/consolidated-implementation-plan.md#oe-d015) — Cross-repo dependency.
- [OE-D049 — Binary build attestations and SBOM](reports/consolidated-implementation-plan.md#oe-d049) — Planned.
- [OE-D050 — Platform signing and notarization](reports/consolidated-implementation-plan.md#oe-d050) — External dependency / decision.
- [OE-D051 — Reproducible build evidence](reports/consolidated-implementation-plan.md#oe-d051) — Research / verification.
- [OE-D052 — CUDA and ROCm release archives](reports/consolidated-implementation-plan.md#oe-d052) — Decision.
- [OE-D053 — Metal in Apple arm64 archive](reports/consolidated-implementation-plan.md#oe-d053) — Decision.
- [OE-D054 — Dynamic backend/CPU dispatch](reports/consolidated-implementation-plan.md#oe-d054) — Deferred candidate.
- [OE-D055 — arm64 and CUDA container images](reports/consolidated-implementation-plan.md#oe-d055) — Decision / candidate.
- [OE-D056 — Windows arm64 build](reports/consolidated-implementation-plan.md#oe-d056) — Research candidate.
- [OE-D057 — Homebrew and winget](reports/consolidated-implementation-plan.md#oe-d057) — Deferred.
- [OE-D058 — Llama 3 RoPE and tokenizer fixes](reports/consolidated-implementation-plan.md#oe-d058) — Planned.
- [OE-D059 — Chat-template metadata and gguf-py rebase](reports/consolidated-implementation-plan.md#oe-d059) — Planned.
- [OE-D060 — Conversion fallback and architecture scope](reports/consolidated-implementation-plan.md#oe-d060) — Owner decision.
- [OE-D061 — Hugging Face GGUF resolver and projector pairing](reports/consolidated-implementation-plan.md#oe-d061) — Owner decision.
- [OE-D062 — Packed NVFP4/MXFP4 source checkpoints](reports/consolidated-implementation-plan.md#oe-d062) — Research candidate.
- [OE-D068 — Strict signed release inventory](reports/consolidated-implementation-plan.md#oe-d068) — Optional security tier.
- [OE-D069 — Release environment and trust verification](reports/consolidated-implementation-plan.md#oe-d069) — Verification / administration.
- [OE-D080 — Action SHA pins and runner hardening](reports/consolidated-implementation-plan.md#oe-d080) — Research candidate.
- [OE-B-Vendor — Publish reusable vendor packages](reports/consolidated-implementation-plan.md#oe-b-vendor) — Optional / conditional.

### O8 — Conformance, robustness and performance evidence

Add official-client contracts, ASan/UBSan/TSan, bounded fuzz corpora and nightly real-model coverage; publish same-build llama-server comparisons at np1/2/4. Separate source risk from reproduced defects.

- [OE-I71 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-003\] Manually verify OpenAI-compatible response formatting](reports/consolidated-implementation-plan.md#oe-i71) — Verification.
- [OE-I70 — \[AI QA\]\[Onyx Engine\]\[SUG\]\[SUG-002\] Manually review shared-state synchronization](reports/consolidated-implementation-plan.md#oe-i70) — QA review / not confirmed defect.
- [OE-D039 — Latency, cache and finish metrics](reports/consolidated-implementation-plan.md#oe-d039) — Planned / research extensions.
- [OE-D040 — Structured access logging and tracing](reports/consolidated-implementation-plan.md#oe-d040) — Research candidate.
- [OE-D042 — Official-client conformance](reports/consolidated-implementation-plan.md#oe-d042) — Planned.
- [OE-D043 — Same-build benchmark suite](reports/consolidated-implementation-plan.md#oe-d043) — Hardware / real models.
- [OE-D044 — Nightly real-model tests](reports/consolidated-implementation-plan.md#oe-d044) — Planned CI.
- [OE-D045 — ASan and UBSan smoke](reports/consolidated-implementation-plan.md#oe-d045) — Planned CI.
- [OE-D046 — Fuzz request and conversion boundaries](reports/consolidated-implementation-plan.md#oe-d046) — Planned CI.
- [OE-D047 — Assert RPC is off](reports/consolidated-implementation-plan.md#oe-d047) — Planned CI.
- [OE-D048 — Static analysis expansion](reports/consolidated-implementation-plan.md#oe-d048) — Research candidate.
- [OE-D078 — ThreadSanitizer concurrency job](reports/consolidated-implementation-plan.md#oe-d078) — Research candidate.
- [OE-D079 — Expanded fuzz scope and Python property tests](reports/consolidated-implementation-plan.md#oe-d079) — Research candidate.

### O9 — NVIDIA profiling and measured scheduling

Implement O1–O4 branch packages in order: baseline profiling, bounded instrumentation, evidence-backed builds, then measured mixed-media scheduling changes.

- [OE-O1 — Nsight profiling workflow](reports/consolidated-implementation-plan.md#oe-o1) — Branch-only planned.
- [OE-O2 — Optional NVTX and safe profiling properties](reports/consolidated-implementation-plan.md#oe-o2) — Branch-only planned / conditional.
- [OE-O3 — NVIDIA build presets](reports/consolidated-implementation-plan.md#oe-o3) — Branch-only planned.
- [OE-O4 — Workload-aware scheduling](reports/consolidated-implementation-plan.md#oe-o4) — Branch-only planned.

### O10 — Research candidates awaiting promotion

Run scoped experiments using current pin; resolve ownership/compatibility/cost; either promote to a bounded PR with tests or record deferral.

- [OE-D037 — Model detail and health aliases](reports/consolidated-implementation-plan.md#oe-d037) — Research candidate.
- [OE-D038 — Fill-in-the-middle code completion](reports/consolidated-implementation-plan.md#oe-d038) — Research candidate.
- [OE-D063 — Idle sleep for other supervisors](reports/consolidated-implementation-plan.md#oe-d063) — Optional candidate.
- [OE-D064 — Cache-aware fairness and media prefill](reports/consolidated-implementation-plan.md#oe-d064) — Research / later.
- [OE-D065 — llguidance](reports/consolidated-implementation-plan.md#oe-d065) — Conflicting proposals / decision.
- [OE-D066 — llama_batch_ext and llama_process migration](reports/consolidated-implementation-plan.md#oe-d066) — Future API research.
- [OE-D070 — Audio transcription API](reports/consolidated-implementation-plan.md#oe-d070) — Low-priority research.
- [OE-D071 — Additional native completion controls](reports/consolidated-implementation-plan.md#oe-d071) — Research candidate.
- [OE-D072 — LoRA adapter hot swap](reports/consolidated-implementation-plan.md#oe-d072) — Research candidate.
- [OE-D073 — Backend/GPU sampler chains](reports/consolidated-implementation-plan.md#oe-d073) — Research candidate.
- [OE-D074 — Additional upstream load and memory flags](reports/consolidated-implementation-plan.md#oe-d074) — Research candidate.
- [OE-D075 — Encoded media cache and prefill batching](reports/consolidated-implementation-plan.md#oe-d075) — Research candidate.
- [OE-D076 — Video and expanded audio input forms](reports/consolidated-implementation-plan.md#oe-d076) — Research candidate.
- [OE-D081 — K-quant and i-quant production](reports/consolidated-implementation-plan.md#oe-d081) — Low-priority research.
- [OE-D083 — Model hashes and optional signatures](reports/consolidated-implementation-plan.md#oe-d083) — Research candidate.
- [OE-D084 — Request priorities with starvation bounds](reports/consolidated-implementation-plan.md#oe-d084) — Research candidate.
- [OE-D085 — Shared system-prompt reuse and disk tier](reports/consolidated-implementation-plan.md#oe-d085) — Research candidate.
- [OE-D086 — Compiled grammar cache and mask scheduling](reports/consolidated-implementation-plan.md#oe-d086) — Research candidate.
- [OE-D087 — Graceful drain and warmup](reports/consolidated-implementation-plan.md#oe-d087) — Research candidate.
- [OE-D088 — Additional reasoning and tool conventions](reports/consolidated-implementation-plan.md#oe-d088) — Research candidate.

## Dependencies owned elsewhere

| Contract | Producer / consumer | What must arrive |
|---|---|---|
| X02 | JX Runtime → Onyx Engine integration | JX routing/capability/type/permission and cancellation support plus managed pin/asset recognition for each newly released Onyx feature. Onyx direct-client implementation/release can proceed; claiming support through JX waits for JX integration and differential streaming/error tests. |
| X03 | Song Deck → JX Runtime + Onyx Engine | Real role schemas/prompts and acceptance cases, capable 8B+ model/device access, browser origins and second-device TLS/pairing walkthrough. Both repos can add fixtures; end-to-end role/schema/pairing claims wait for Song Deck exercising those exact contracts and retaining results. Song Deck application logic remains there. |
| X05 | QAI-ality ↔ JX Runtime / Onyx Engine | QAI-ality config/apps.yml enables JX browser QA with a deterministic startup/auth contract and disables irrelevant browser QA for headless Onyx. Repo-local test fixtures and qa-check diagnostics can proceed; automated browser-level completion waits for a successful QAI-ality run with the correct configuration. |
| X08 | Onyx Engine ↔ BackTrack | Coordinated ggml source pin update and check-ggml-pin.sh results against BackTrack packaging/vendor/ggml.conf. Onyx pin upgrade can be reviewed independently; claiming cross-repo pin alignment waits for BackTrack change and validation. Current BackTrack state must be inspected then. |
| X09 | Onyx Engine + shared release infrastructure → JX Runtime / audio apps | Stable release convention, supported asset variants/layout, checksums, optional attestation identity/trust-root protocol and working publisher. Repo-local packaging and verifier fixtures can proceed; live download/attestation acceptance waits for actual published artifacts. Shared-toolkit fixes belong in infrastructure, not copied into consumers. |
| X10 | AI Server Studio ↔ JX Runtime ↔ Onyx Engine | Versioned execution-profile, setup-plan and safe profiling properties contracts (branch C1/C2/C3/C4/C6), explicit public/admin boundaries and unknown-field fallback. Developer measurement recipes can proceed independently. Profile/setup UI integration waits for agreed schemas and Studio consumption; Onyx optional properties must not break old JX. |
| X12 | llama.cpp / ggml / cpp-httplib upstream → Onyx Engine and JX capability tables | Stable or deliberately pinned API support and security fixes; especially MTP/EAGLE3/DFlash state/rollback, backend loading, extended fit/batch interfaces. Ordinary fixes and supported flags can proceed. Experimental APIs stay disabled until the selected pin exposes and passes the required contract. |
| X13 | JX Runtime → Onyx documentation cleanup | JX #327 confirms no links to the four moved Onyx docs and a matching integration contract; this producer requirement is satisfied. Onyx can evaluate retiring compatibility stubs after checking other consumers. JX is no longer a blocker; this does not authorize deleting unrelated docs. |

## Scheduling and completion policy

Reliability and truthful contracts come before widening API/default behavior. Independent local work may proceed while an integration is blocked; the implementation plan names the exact wait. Hardware, signing identities, independent review, contributor participation and account access are resource gates, not pretend repo changes.

Research, optional features and branch-only packages remain separately labeled. Promote them only after a recorded decision and scoped acceptance criteria. Defer or reject explicitly rather than silently removing them from the inventory. Do not schedule another repo’s work as if it were completed here.

When a package lands, record its PR/release and evidence in the implementation plan, then update this roadmap’s baseline and remaining scope. Keep historical plans as sources with supersession notes; there is one active implementation index.
