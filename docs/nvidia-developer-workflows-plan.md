# NVIDIA developer workflows — Onyx Engine plan

Date: 2026-10-21. Status: accepted direction; all packages below are planned.

[Coordinated plan and C1–C6 contracts](https://github.com/jxburros/AI-Server-Studio/blob/docs/nvidia-developer-workflows/development-docs/architecture-plans/nvidia-developer-workflows/README.md) ·
[Studio plan](https://github.com/jxburros/AI-Server-Studio/blob/docs/nvidia-developer-workflows/development-docs/architecture-plans/nvidia-developer-workflows/studio-plan.md) ·
[JX plan](https://github.com/jxburros/JX-Runtime/blob/docs/nvidia-developer-workflows/development-docs/architecture-plans/nvidia-developer-workflows-plan.md)

## Scope and existing foundations

Onyx remains a portable, single-model inference engine. NVIDIA tooling improves engineering
and optional CUDA builds; it does not turn Onyx into a model router, agent framework,
container manager or TensorRT-LLM wrapper.

Continuous batching, text-prefill chunking and prefix reuse already exist. Instrument and
improve them rather than planning them as new capabilities. Current media prefill can occupy
the engine loop; `--mmproj` disables cache reuse/context shift. Do not promise media-aware
cache reuse, arbitrary safetensors architectures or unrelated scheduler rewrites here.

Start with `src/engine.cpp`, `src/engine.h`, `src/server.cpp`, `src/args.cpp`, `src/args.h`
and `CMakeLists.txt`. Preserve the documented [JX integration contract](jx-runtime-integration.md),
[API](api.md), [architecture](architecture.md) and [build paths](building.md).
Coordinate touched engine code with the existing
[improvement report](../reports/Onyx%20Engine%20improvement%20opportunities.md) and
[common-library replacement plan](../reports/Replacing%20llama%20cpp%20common%20code.md).
Neither wholesale `common/` removal nor speculative decoding is a prerequisite for this work.
If either lands first, attach instrumentation to the resulting engine seam rather than
reintroducing removed dependencies.

## Ranked work packages

| ID | Action | Deliverable | Complexity | Dependencies and cross-references |
|---|---|---|---|---|
| O1 | Implement + emulate | Nsight recipes and a profiling-first workflow | 2/5 | Existing build/smoke inputs; C2 with JX J1 |
| O2 | Implement | Optional NVTX ranges and safe engine observations | 3/5 | C1/C2 agreement with JX J2; O1 baseline |
| O3 | Option | NVIDIA-specific build/performance presets | 3–4/5 | O1/O2 evidence; C6 for JX managed binaries |
| O4 | Emulate | Workload-aware scheduler improvements | 4/5 | O1/O2 baseline; C2 with JX J1 and Studio S14 |

## O1 — reproducible profiling

1. Add documented developer recipes for Nsight Systems, then targeted Nsight Compute
   sessions only after a system trace identifies a kernel bottleneck. Document installation,
   profiler permissions, GPU/platform support and the exact Onyx build/model configuration.
2. Define small public/synthetic workloads for single-user latency, long prefill, concurrent
   decoding, repeated-prefix reuse and mixed text/media where supported. Include CPU-side
   queueing/serialization observations rather than treating every delay as a GPU issue.
3. Save a local report manifest with binary/model digest, compiler/CUDA/driver/GPU details,
   context per slot and total, slot count, batch settings, warmup and run parameters.
   Do not commit private traces or prompt text. Explain trace overhead and privilege needs.
4. Establish the engineering loop: baseline → identify bottleneck → bounded change → repeat
   the same workload → retain/revert based on correctness and measured tradeoffs.

**Done when:** recipes are reproducible on supported NVIDIA hardware and identify what was
measured. If hardware is unavailable, ship the recipes as unvalidated tooling and retain
that limitation. Benchmark schema alignment with JX J1 is **C2**; no Studio change is required.

## O2 — optional instrumentation and execution facts

1. Add a build-time profiling option with NVTX ranges for admission/queue wait, prefill,
   decode, cache lookup/reuse, media processing and cancellation observation where measurable.
   Keep disabled-build behavior lightweight and free of an NVIDIA runtime requirement.
   Use bounded IDs/counters, never prompts, paths, credentials or document content as labels.
2. Define metric units, boundaries, reset behavior and unavailable values before exporting
   them. Separate current gauges from cumulative counters; do not fabricate per-request
   data from global totals. Bound retention and label cardinality. Measure instrumentation
   overhead before recommending it for normal execution.
3. Add only the additive engine properties/diagnostics needed by JX J2. Agree transport
   and exposure with JX first; do not make private internal endpoints publicly reachable
   through the runtime. Keep `/props` context per slot distinct from `n_ctx_total`, retain
   model aliases and preserve existing OpenAI response/SSE behavior.
4. Keep the CLI parser and `--help` synchronized for any new switch. Version the new schema,
   document capabilities and preserve old adapter behavior when new fields are absent.

**Done when:** JX can ingest supported facts without assuming them on old builds, optional
profiling compiles out cleanly, and cancelling a request produces truthful lifecycle evidence.
Review **C1/C2** before schema implementation and **C6** before JX changes its managed pin.
Studio consumes these facts through JX, not through a new direct privileged engine connection.

## O3 — optional NVIDIA build tuning

1. Use O1/O2 to select a real bottleneck and a bounded existing ggml/CUDA/compiler setting.
   Prefer upstream-supported options before custom kernels. Record supported compute
   capabilities, toolkit requirements, build time, binary size and fallback behavior.
2. Create opt-in build presets with explicit GPU architecture coverage. Preserve existing
   CPU, Metal, Vulkan and ROCm builds where supported; do not make CUDA mandatory or
   require NVIDIA profiling headers in every build.
3. Compare equivalent builds/configurations for correctness, latency, throughput and memory.
   Keep a change optional if evidence is narrow or shows workload regressions; do not
   describe a local improvement as a universal speedup.
4. If distributing a new artifact variant, coordinate its asset identity, checksum, loader
   prerequisites and compatibility metadata with JX's managed installer before publication.
   Preserve established asset names unless a deliberate versioned migration is necessary.

**Done when:** supported variants have recorded hardware evidence and a working fallback;
unsupported GPUs get a useful error instead of an unexplained loader failure. JX J2 consumes
build compatibility, and **C6** covers managed distribution. Studio requires no code change
unless a new selectable execution property is exposed.

## O4 — workload-aware scheduling

1. Measure mixed short/long text and text/media requests first. Identify queueing, media
   prefill stalls, decode starvation and cancellation latency separately. Keep existing
   batching/prefix-reuse behavior as the baseline, with cold and warm paths distinguished.
2. Implement the smallest supported scheduler change: bounded prefill work, fair admission
   or safe media-work staging. Check mtmd/llama context ownership and thread-safety before
   moving work off the engine loop. If chunking media work is unsupported by the library,
   document the constraint and use supported admission/fairness controls instead.
3. Preserve request ordering, token accounting, context limits, cancellation, slot cleanup
   and output correctness. Any configurable policy must remain mechanical (queue/fairness/
   resource behavior), never inspect prompts to choose semantic task importance.
4. Compare p50/p95 latency, TTFT, throughput, cancellation latency and memory under the
   same resolved workload. Report regressions and limit enablement to the validated cases.

**Done when:** an identified fairness/latency problem improves under recorded load without
silent correctness loss or unsupported cache reuse. Coordinate measurements at **C2** and
new flags/properties at **C1/C6**. JX passes explicitly supported settings; Studio S14 can
recommend them from evidence. Onyx does not implement Studio's selection policy.

## First implementation handoff and completion

Start O1 alongside the C1/C2 discussion for O2. Agree metric meanings before adding output
fields that JX would have to normalize later. Split instrumentation from optimization so
the baseline remains reviewable. Use existing native build/smoke coverage and relevant
correctness checks for engine changes; real GPU performance needs real hardware evidence.
Update this plan's status, current API/build/integration docs and changelog as work lands.
Follow the manual release process and advance versions before merge according to repository
policy; this planning change does not publish binaries or update JX's pinned version.
