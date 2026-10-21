# State-of-the-art LLM serving techniques applicable to Onyx Engine (as of Oct 2026)

Context for the reader. Onyx Engine (per `/home/user/Onyx-Engine/docs/architecture.md`) is a single-model C++ server built on llama.cpp. It runs one engine thread that does continuous batching across `-np` slots. Each step packs one prompt chunk per prefilling slot plus one decode row per generating slot into a single `llama_decode`. `kv_unified=false`, so `n_ctx` is split evenly per slot. Prefix reuse is a longest-common-prefix (LCP) match against each idle slot's last prompt only. Admission is FIFO with no queue limit. Optional context shift exists, and `--mmproj` disables cache reuse and context shift. There is no speculative decoding (README "not supported" list). Upstream `llama-server` has since added most of the features below, which makes it the most directly portable reference implementation, since Onyx links the same `llama.h`.

Source-quality note: llama.cpp PRs and manpages, the vLLM docs, the Ollama FAQ and arXiv papers are primary sources. Most consumer-hardware speedup numbers come from blogs or community posts and are labelled as anecdotal.

## Prefix caching: radix tree, hashed blocks, multi-tier KV, and multi-prompt caching across slots

### Takeaway
Datacenter engines cache many past prefixes. SGLang uses an LRU radix tree, and vLLM uses chained block hashes with LRU eviction. Upstream llama.cpp got a practical local analogue in Oct 2025: a host-RAM prompt cache (`--cache-ram`, default 8 GiB) that acts as "extra slots" and is hot-swapped into the context, plus `--kv-unified`. For Onyx, the cheapest high-value step is to port that design. It would turn "one remembered prompt per slot" into "N remembered prompts in RAM", which is a large time-to-first-token (TTFT) win for agent and multi-conversation workloads. Building a true block-level radix tree would require PagedAttention-style KV that llama.cpp does not expose.

### Cited Findings
- **SGLang RadixAttention.** It keeps an LRU cache of KV entries in a radix tree that supports match, insert and evict operations. New requests are prefix-matched, and unmatched suffixes get new nodes. — [SGLang paper, arXiv 2312.07104](https://arxiv.org/pdf/2312.07104); [LMSYS blog, Jan 2024 (older)](https://lmsys.org/blog/2024-01-17-sglang)
- **SGLang cache-aware scheduling.** It orders waiting requests by matched-prefix length. The paper proves that depth-first traversal of the radix tree gives the optimal hit rate when the cache is at least the maximum request length. — [arXiv 2312.07104](https://arxiv.org/pdf/2312.07104)
- **Reported SGLang hit rates.** Cache hit rates of 50% to about 99% are reported across benchmarks. This comes from a tutorial citing the paper; its other numbers (6.4x on RAG) could not be verified. — [generalcompute blog](https://www.generalcompute.com/blog/sglang-and-radix-attention)
- **vLLM Automatic Prefix Caching (APC), block hash.** Each full block's hash = hash(parent block hash, block tokens, extra hashes). Extra hashes include LoRA ID, multimodal input hash and cache salt. Only full blocks are cached. — [vLLM prefix caching design](https://docs.vllm.ai/en/latest/design/prefix_caching.html)
- **vLLM APC, hash algorithm.** The default is sha256 since v0.11. xxhash is offered but warned about for collision risk in multi-tenant settings. — [vLLM design doc](https://docs.vllm.ai/en/latest/design/prefix_caching.html)
- **vLLM APC, free list and eviction.** Free blocks sit in an O(1) doubly-linked list with LRU eviction. A finished request's blocks are appended in reverse order, so the tail block is evicted first. Cache-hit blocks are "touched" (removed from the free queue) before new allocation. — [vLLM design doc](https://docs.vllm.ai/en/latest/design/prefix_caching.html)
- **vLLM APC, multimodal.** The image hash is mixed into every block containing that image's placeholder tokens, so different images never collide. — [vLLM design doc](https://docs.vllm.ai/en/latest/design/prefix_caching.html)
- **vLLM APC, cache salt.** An optional per-request `cache_salt` is mixed into the first block hash. Only requests with the same salt share blocks, which mitigates timing side channels. — [vLLM design doc](https://docs.vllm.ai/en/latest/design/prefix_caching.html)
- **llama.cpp host-RAM prompt cache (PR #16391, merged Oct 9, 2025).** Cached prompts in regular RAM act as "extra slots". The server computes prefix similarity against them and hot-swaps one into the `llama_context` when that reduces processing. It has two limits: bytes (`--cache-ram`/`-cram`) and cached tokens (default = context size). It is stated to work with a single slot and with dense, MoE, SWA and SSM models. It is incompatible with mtmd because `server_tokens` isn't copyable. The PR itself has no benchmarks. — [llama.cpp PR #16391](https://github.com/ggml-org/llama.cpp/pull/16391)
- **Downstream report.** One downstream project reported an 11.7k-token prompt dropping from 9.9 s cold to 0.97 s cached. This is a third-party commit quoted on the PR page, not the PR's own measurement. — [llama.cpp PR #16391 page](https://github.com/ggml-org/llama.cpp/pull/16391)
- **Current llama-server flags (manpage, llama.cpp-tools 0.7.0).**
  - `--cache-ram` defaults to 8192 MiB (-1 = unlimited, 0 = off).
  - `--kv-unified` (one shared KV buffer across sequences) is enabled when slot count is auto.
  - `--kv-unified-per-slot N` sets a per-slot cap within the shared pool.
  - `--slot-prompt-similarity` defaults to 0.10.
  - `--slot-save-path` saves slot KV to disk.
  - `--cache-reuse N` (default 0) reuses chunks via KV shifting.
  - `--ctx-checkpoints` defaults to 32 per slot, with `--checkpoint-min-step` 8192.

  — [Debian manpage llama-server(1)](https://manpages.debian.org/unstable/llama.cpp-tools/llama-server.1.en.html)
- **LMCache.** It stores KV across GPU, CPU DRAM and local disk for multi-turn and RAG reuse. A vendor-side claim of 3–10x latency reduction with vLLM is reported without methodology. — [Modular handbook](https://handbook.modular.com/inference-optimization/kv-cache-offloading/)
- **Offload tier speed.** If the offload tier is too slow, the transfer back can negate the benefit compared with recomputing. — [BentoML handbook](https://bentoml.com/llm/inference-optimization/kv-cache-offloading)

### Inferences
- **Recommended port for Onyx: a host-RAM prompt cache like llama-server's.**
  - On request completion (or slot eviction), save the slot's sequence state to RAM using llama.cpp's per-sequence state API (`llama_state_seq_get_data`/`set_data` family), keyed by `cache_tokens`.
  - On admission, compute the LCP against both idle slots and the RAM entries. If a RAM entry beats the best slot by a margin, restore it into the chosen slot's sequence and decode only the suffix.
  - Use LRU eviction under a byte cap.
  - Expected benefit: multi-conversation or agent clients that interleave more distinct conversations than `-np` stop paying full re-prefill. On CPU, where prefill is the dominant cost, this can turn tens of seconds into about a second, per the ~10x downstream anecdote.
  - The PR did not document whether upstream uses per-seq state save/load; verify in `tools/server` source.
- **Unified KV (`kv_unified=true`).** This removes Onyx's main structural inefficiency: `n_ctx / np` per slot means one long request cannot use idle slots' memory. Slots would then share one pool, with `kv_unified_per_slot`-style caps. Admission must then check free cells rather than "an idle slot exists", and must handle a request failing to find space (preempt or queue).
- **Cache-aware scheduling.** Onyx already picks the idle slot with the longest LCP. A cheap SGLang-style extension is to let a queued request with a long cache hit jump ahead of the FIFO head, bounded by a starvation limit.
- **Radix tree or hashed blocks.** Without paged KV in llama.cpp, a full radix tree sharing physical KV between live sequences isn't feasible. The closest substitute is copying a shared system-prompt prefix with `llama_memory_seq_cp` into a fresh slot, then decoding the suffix. That gives an "N slots share one cached system prompt" effect at the cost of KV duplication, not compute.
- **Hashing and multimodal.** vLLM's chained-hash idea is useful as a key for the RAM or disk cache, and for multimodal, where mixing an image hash into the key would let Onyx re-enable prefix caching for mtmd prompts it currently disables. Upstream still had mtmd excluded from the RAM cache as of the PR.
- **Disk tier.** `--slot-save-path`-style save to disk is a cheap "LMCache-lite" for long fixed documents or system prompts that survive restarts. Its value depends on whether disk read beats recompute, which it usually does on CPU-only machines.

### Gaps
- No independent benchmark of llama-server `--cache-ram` hit rates or TTFT savings was found. No measured LMCache CPU-vs-disk TTFT comparison was found.
- The exact upstream eviction policy and restore mechanism for `--cache-ram` were not confirmed from the PR text; only third-party text mentions LRU.

## Scheduling: chunked prefill, token budgets, priority/fairness, preemption, admission control and backpressure

### Takeaway
The consensus design is "decode-first plus a per-step token budget filled with prefill chunks" (Sarathi-Serve, vLLM V1). It bounds inter-token latency (ITL) for running streams while a long prompt prefills. Onyx already mixes prefill chunks with decode rows. What it most likely lacks is an explicit per-tick token budget, priorities, a queue cap with 503/429, and preemption. All of these are pure scheduler logic and feasible in Onyx's single-thread loop.

### Cited Findings
- **Sarathi-Serve.** Chunked prefill splits a prompt into equal compute-sized chunks over iterations. "Stall-free scheduling" lets new requests join without pausing decodes. It reports up to 2.6x throughput within SLO for Mistral-7B on one A100, versus Orca and vLLM. — [Microsoft Research abstract](https://microsoft.com/en-us/research/?p=1032951); [arXiv 2403.02310](https://arxiv.org/html/2403.02310v3)
- **Fixed token budget.** Sarathi-Serve is summarized as a fixed per-batch token budget that prioritizes decodes. — [arXiv 2508.01002](https://arxiv.org/pdf/2508.01002)
- **vLLM V1 chunked prefill.** It is always on by default in V1. The scheduler batches all decodes first, then fills the remaining `max_num_batched_tokens` with prefills, chunking any that don't fit. — [vLLM Optimization docs v0.10.2](https://docs.vllm.ai/en/v0.10.2/configuration/optimization.html); consistent through [v0.19.0](https://docs.vllm.ai/en/v0.19.0/configuration/optimization/)
- **Token budget size.** A smaller budget (e.g. 2048) improves ITL, and a larger one improves TTFT; above 8192 is recommended for throughput. — [vLLM docs](https://docs.vllm.ai/en/v0.19.0/configuration/optimization/)
- **vLLM V1 preemption.** The default preemption mode is RECOMPUTE rather than SWAP, because recomputation is cheaper in V1. — [vLLM docs](https://docs.vllm.ai/en/v0.10.2/configuration/optimization.html)
- **Ollama queue backpressure.** Ollama queues FIFO beyond `OLLAMA_NUM_PARALLEL` (default 1). `OLLAMA_MAX_QUEUE` defaults to 512, and a full queue returns 503 "overloaded". — [Ollama FAQ](https://docs.ollama.com/faq)
- **llama-server `--prio`.** It sets process or thread scheduling priority (low to realtime), not request priority. — [manpage](https://manpages.debian.org/unstable/llama.cpp-tools/llama-server.1.en.html)

### Inferences
- **Token budget.** Add `--max-batch-tokens` (or reuse `n_batch`) as a per-tick budget: all decode rows first, then prefill chunks, either round-robin or shortest-remaining-first. This caps the tick duration, so streaming ITL stays smooth when a 30k-token prompt arrives. The benefit is largest on CPU, where a single unchunked big prefill tick can stall other streams for seconds.
- **Backpressure.** Add `--max-queue N` and return 503 with `Retry-After` (Ollama semantics), or 429 if per-client limits are added later. Also add a queue-wait timeout. This is trivial to implement and matters for JX Runtime callers that need to detect overload.
- **Priority.** Accept a request priority field (vLLM has a priority scheduling policy, but it was not documented in the fetched pages), so that interactive requests jump ahead of batch or embedding work. This only reorders the queue; preemption is optional.
- **Preemption.** Only meaningful after a unified KV is introduced, when memory, not slots, becomes the constraint. Following vLLM V1, recompute-based preemption (drop the KV and re-queue with the prompt plus generated tokens) is simpler than swap. With a RAM prompt cache it becomes "swap" for free.
- **Note for Onyx.** Since it already packs a prompt chunk per prefilling slot, verify whether that chunk size is bounded per tick in total. If several slots prefill at once, the effective tick size is the sum of their chunks.

### Gaps
- Not found: SLO-aware scheduling implementations targeting local servers, and vLLM's priority-policy semantics (not covered in the fetched docs).
- The Sarathi paper's budget-size sweep was not retrieved.

## Speculative decoding variants and measured speedups on consumer hardware

### Takeaway
Upstream llama.cpp (2026) has draft-model, n-gram (`ngram-cache`/`ngram-mod`) and MTP (`draft-mtp`, merged May 2026) speculation under `--spec-type`. Gains are strongly workload-dependent: about 1.5–2x for well-matched pairs or MTP on code and structured output, and slowdowns at low acceptance. N-gram/prompt-lookup is the cheapest to add, needs no extra weights, works with parallel slots, and excels at edit/rewrite/agent workloads (SuffixDecoding shows 2–4.5x on agentic traces in vLLM). EAGLE-3 is reportedly landing in llama.cpp but is unconfirmed.

### Cited Findings
- **llama.cpp `--spec-type`.**
  - Types: draft-*, ngram-*.
  - `--spec-draft-n-max` default 3; `-n-min` 0; `-p-split` 0.10; `-p-min` 0.00.
  - `--spec-draft-sampling` is greedy or probabilistic.

  — [manpage](https://manpages.debian.org/unstable/llama.cpp-tools/llama-server.1.en.html)
- **Cleanup PR #23269 (merged May 19, 2026).** It changed the default n-max from 16 to 3, made `p-min` functional, added ngram-k4v to `--spec-default`, and fixed parallel decoding with recurrent rollback. — [llama.cpp PR #23269](https://github.com/ggml-org/llama.cpp/pull/23269) (via search summary); MTP merge date (May 16, 2026) from secondary [mer.vin](https://mer.vin/2026/05/run-qwen-3-6-mtp-in-llama-cpp-faster-local-inference-with-built-in-speculative-decoding/)
- **HF 128K-context benchmark (Qwen3.6 MTP-capable model; hardware unstated).**

  | Mode | Prefill | Decode | Acceptance |
  |---|---|---|---|
  | Baseline | 3,840 tok/s | 90 tok/s | n/a |
  | MTP, n=2 | 2,978 (−22%) | 95 (+5%) | 76% |
  | N-gram cache, n=2 | +1% | 75 (−17%) | 32% |

  — [HF Space "Speculative decoding in llama.cpp — MTP vs the others"](https://victor-mtp-on-hf-endpoints.static.hf.space/index.html)
- **Same source, integration caveats.**
  - In llama.cpp, MTP silently disables `nParallel > 1` and `--cache-reuse`, and adds a D2H/H2D pre-norm embedding transfer per ubatch that "kills prefill at long context".
  - N-gram and draft model both work with parallel slots and cache reuse.

  — [HF Space](https://victor-mtp-on-hf-endpoints.static.hf.space/index.html)
- **Consumer and edge numbers compiled by a 2026 blog (anecdotal).**

  | Setup | Hardware | Result |
  |---|---|---|
  | Qwen3.6-27B MTP | A10G | ~25 → ~45 tok/s |
  | Qwen3.6-27B MTP | Apple Silicon | ~7 → ~16 tok/s (82% acceptance) |
  | Llama 3.1 8B + 3.2 1B draft | not stated | 1.83x |
  | 8B + 1B draft | RTX 4090 | ~130 → >180 tok/s |
  | Gemma 4 MTP, coding agent | MLX | ~1.9x |
  | Poorly matched int4 Gemma 4 draft (41% match) | not stated | 60 → 45 tok/s |

  The blog estimates losses below roughly 60–70% acceptance. It also notes that a drafter can push the target into CPU offload on 8–12 GB cards, and that gains are mainly single-stream. — [runaihome.com blog](https://runaihome.com/blog/speculative-decoding-llama-cpp-local-llm-setup-2026/)
- **EAGLE-3.** Up to 6.5x speedup, about 1.4x over EAGLE-2, on 2x RTX 3090 with Vicuna 13B fp16 in the authors' framework. In SGLang it gives 1.38x throughput at batch 64. A vc.ru article claims NVIDIA and ggml added EAGLE3 to llama.cpp with a 2–3x speedup; this is unverified. — [EAGLE repo mirror](https://gitee.com/slqgenius/EAGLE); [NeurIPS 2025 poster](https://neurips.cc/virtual/2025/poster/119930); [vc.ru](https://vc.ru/ai/2977323-optimizatsiya-inferensa-s-eagle3-v-llama-cpp)
- **SuffixDecoding.** A model-free method using suffix trees over prompts and prior outputs, with adaptive speculation length. On SWE-Bench/OpenHands in vLLM it gives 1.8–4.5x over vanilla and 1.3–3x over prompt-lookup decoding (PLD). Headline maxima differ by version: up to 5.3x (arXiv v2) versus up to 3.9x (NeurIPS). — [arXiv 2411.04975](https://arxiv.org/html/2411.04975v2); [NeurIPS 2025](https://neurips.cc/virtual/2025/poster/115453)
- **SuffixDecoding production (Snowflake, Dec 2025).** 1.96–3.12x on BlazeEdit and 1.0–1.28x on SpecBench, described as production-ready at all concurrencies, in ArcticInference. — [Snowflake blog](https://www.snowflake.com/en/engineering-blog/suffixdecoding-arctic-inference-vllm/)

### Inferences
- **Phase 1 for Onyx: n-gram/prompt-lookup speculation.** Draft from a per-slot n-gram map over prompt and output, verify k tokens as extra batch rows of the same `seq_id`, then `llama_memory_seq_rm` the rejected tail.
  - It fits Onyx's single batch: speculative rows just add rows per slot.
  - No extra memory, and it composes with `-np`.
  - Make it adaptive: disable per slot when the acceptance EMA is low, to avoid the −17% case.
  - High value for code-edit, RAG-quote and agent tool-loop traffic.
  - A SuffixDecoding-style global suffix tree across requests is a natural upgrade for agent workloads that repeat outputs.
- **Phase 2: a draft model (second `llama_context`).** It works with parallel slots, but costs memory. It is of modest value on CPU-only systems, where both models compete for memory bandwidth.
- **MTP.** Attractive for Qwen3.6/GLM/DeepSeek-family models (75–90% acceptance). But upstream's implementation currently forces single-slot and hurts long-context prefill, so Onyx should wait or gate it to `np == 1`.
- **Expected benefit.** Speculation helps single-stream decode latency, which is the common local case. Benefit shrinks as `-np` load rises, because decode is already batched.

### Gaps
- No primary benchmark of llama.cpp speculation on CPU-only hardware was found; all CPU and Apple numbers are anecdotal.
- EAGLE-3 merge status in llama.cpp is unconfirmed.
- Medusa was not researched further. It is largely superseded by EAGLE and MTP in 2025–26 sources (inference, not sourced).

## KV cache quantization, sliding-window/hybrid model memory, and MoE expert offloading

### Takeaway
q8_0 KV is a near-free 2x KV memory saving on most models, while q4_0 is model-dependent. Gemma's SWA models were notably hurt. It requires flash attention in Ollama and llama.cpp. For SWA/hybrid/recurrent models, upstream llama.cpp uses context checkpoints to keep prefix reuse working where `seq_rm` truncation isn't possible. MoE offloading (`--n-cpu-moe`, `-ot`) is the key enabler for running 30B–600B MoE models on consumer GPUs. KTransformers shows bigger gains with AMX/AVX-512 kernels, but that is a separate engine.

### Cited Findings
- **llama-server KV cache types.** `-ctk`/`-ctv` accept f32, f16, bf16, q8_0, q4_0, q4_1, iq4_nl, q5_0 and q5_1 (default f16). — [manpage](https://manpages.debian.org/unstable/llama.cpp-tools/llama-server.1.en.html)
- **Ollama's KV quantization guidance.** q8_0 uses about 1/2 the memory of f16 with very small quality loss and is recommended. q4_0 uses about 1/4, with small-to-medium loss that is more visible at long context. It takes effect only with Flash Attention, and high-GQA models (e.g. Qwen2) may be more affected. — [Ollama FAQ](https://docs.ollama.com/faq)
- **KL-divergence benchmark (2026 blog).** Qwen models stay below KL 0.04 at q8_0 and 0.087–0.117 at q4_0. A Gemma 26B-A4B variant at q4_0 reached KL 1.088 with 68% top-1 agreement. The SWA-cache explanation is commenter speculation. — [localbench substack](https://localbench.substack.com/p/kv-cache-quantization-benchmark)
- **Perplexity can hide changes.** A DeepSeek V4 Flash q8_0 KV test showed near-unchanged perplexity but changed token choices. — [dev.to](https://dev.to/breachprotocol/quantizing-v4-flashs-kv-cache-in-llamacpp-changes-which-tokens-it-picks-38np)
- **SWA cache and checkpoints.** llama-server has `--swa-full` (full-size SWA cache, default false) and context checkpoints (`--ctx-checkpoints` 32 per slot, `--checkpoint-min-step` 8192). The host-RAM prompt cache is stated to work with SWA and SSM models. — [manpage](https://manpages.debian.org/unstable/llama.cpp-tools/llama-server.1.en.html); [PR #16391](https://github.com/ggml-org/llama.cpp/pull/16391)
- **MoE offload flags.** `--n-cpu-moe N` keeps the MoE expert weights of the first N layers on CPU, and `--cpu-moe` keeps all of them there. `-ot <pattern>=<buffer type>` overrides tensor placement. — [manpage](https://manpages.debian.org/unstable/llama.cpp-tools/llama-server.1.en.html); [server README mirror](https://huggingface.co/OpenTransformer/llama.cpp-prismml/blob/main/tools/server/README.md)
- **Example.** Qwen3.6-35B-A3B fit on a 16 GB RTX 3080 via partial offload. — [autodidacts.io](https://www.autodidacts.io/how-to-fit-qwen3-6-35b-a3b-into-16gb-vram-run-with-llama-cpp-rtx-3080/)
- **KTransformers (SOSP'25).** Attention and KV stay on GPU while experts run on CPU with AMX/AVX-512 kernels, which runs DeepSeek-R1 671B on one 24 GB GPU plus RAM. It reports 4.62–19.74x prefill and 1.25–4.09x decode over existing systems. "Expert Deferral" adds up to 1.45x throughput with ≤0.5% accuracy drop. — [MADSys page](https://madsys.cs.tsinghua.edu.cn/publication/ktransformers-unleashing-the-full-potential-of-cpu/gpu-hybrid-inference-for-moe-models/); [noze.it summary](https://www.noze.it/en/insights/ktransformers-hybrid-cpu-gpu-inference/)
- **DALI (independent comparison).** It reports DeepSeek prefill about 3.80x faster than KTransformers and 7.62x faster than llama.cpp, citing KTransformers' static layer-wise mapping. — [arXiv 2602.03495](https://arxiv.org/pdf/2602.03495)

### Inferences
- **KV type flags.** If Onyx doesn't yet expose `--cache-type-k/v`, it is a one-line pass-through to `llama_context_params.type_k/type_v`; require or auto-enable flash attention. Recommend q8_0 as the documented "more slots or context for free" option, and warn about q4_0 on SWA models.
- **Hybrid and SWA prefix reuse.** Onyx currently force-disables context shift for SWA and relies on `seq_rm` for LCP reuse. For SWA, hybrid and recurrent models, `seq_rm` to an arbitrary position may fail or be impossible. Upstream's checkpoint mechanism (periodic per-sequence state snapshots, restore the nearest earlier checkpoint) is the pattern to copy if Onyx targets Qwen3.5/3.6-hybrid, Gemma or Mamba-style models.
- **MoE offload.** Expose `--n-cpu-moe`/`-ot`, or the `tensor_buft_overrides` passthrough, in Onyx args. This is high value for local MoE users and only plumbing. KTransformers-style kernels are out of scope; llama.cpp's CPU MoE path is what Onyx inherits.

### Gaps
- No primary llama.cpp-maintainer measurement of KV quantization quality was found; the data comes from community benchmarks.
- No current data was found on llama.cpp vs KTransformers decode on the same consumer hardware.

## Structured generation engines (XGrammar, llguidance, Outlines) vs GBNF

### Takeaway
llama.cpp's built-in GBNF sampler is the slow baseline in both the XGrammar and llguidance literature. llguidance is about 50 µs/token for mask computation, and XGrammar under 40 µs/token with precompute. llama.cpp can already be built with llguidance (`LLAMA_LLGUIDANCE`). For Onyx, enabling that build option and routing `json_schema`/`response_format` through it is the practical upgrade. The gain matters most for large-vocabulary models, complex schemas and multiple parallel constrained slots on a single CPU thread.

### Cited Findings
- **XGrammar paper.** It compares against llama.cpp b3998 on Llama-3.1-8B: under 40 µs/token for JSON Schema and CFG, up to 3x faster on JSON Schema and over 100x on CFG versus the best baseline. — [arXiv 2411.15100](https://arxiv.org/pdf/2411.15100)
- **llguidance.** About 50 µs single-core CPU per token for a 128k tokenizer, with p99 at 0.5 ms and p100 at 20 ms on JSON Schema Bench. Its maintainers attribute llama.cpp grammar slowness to the lack of a lexer and a backtracking parser. — [llama.cpp llguidance doc (mirror)](https://huggingface.co/OpenTransformer/llama.cpp-prismml/blob/main/docs/llguidance.md); [llguidance docs mirror](https://www.gitlink.org.cn/dnrops/llguidance)
- **XGrammar precompute.** XGrammar is under 8 µs for half of cases when precompute applies, but can take tens to hundreds of ms otherwise, and precompute may take seconds to minutes. This is the llguidance maintainers' characterization. — [llguidance PyPI mirror](https://simple-repository.app.cern.ch/project/llguidance)
- **2026 academic comparisons are mixed.** One paper finds LLGuidance has lower mask overhead than XGrammar on Java, SQL and JSON Schema. Another (Qwen3-8B) finds LLGuidance at 73–141 µs/step versus 5–10 µs for XGrammar, but with the fastest compilation. — [arXiv 2608.03065](https://arxiv.org/pdf/2608.03065); [arXiv 2608.12574](https://arxiv.org/pdf/2608.12574)

### Inferences
- llguidance's near-zero compile time suits per-request ad-hoc JSON schemas, which is the common OpenAI-API pattern. XGrammar's precompute favors repeated schemas; a schema-keyed compiled-grammar cache would help either way.
- In Onyx's single engine thread, sampling for all slots is serialized. A slow GBNF mask on one slot stalls every slot's next token, so faster grammar engines also protect other streams' ITL. Consider computing grammar masks for slots in parallel, or overlapped with `llama_decode`, as XGrammar and SGLang do.
- Outlines was not separately researched. It is a Python and regex/FSM-based library, less relevant to C++ embedding (inference).

### Gaps
- No head-to-head llguidance vs GBNF benchmark inside llama.cpp on the same machine was found.

## Local-first runtime conveniences: keep-alive/idle unload, auto GPU-layer fitting, warmup, graceful drain

### Takeaway
Ollama's 5-minute `keep_alive` unload and 512-deep queue with 503 are the de facto local UX. llama-server now has `--fit` (auto-fit unset args to device memory, on by default), `-ngl auto`, `--warmup`, `--sleep-idle-seconds`, a router mode, `--metrics` and slot save/restore. Each is cheap for Onyx to adopt, though the JX Runtime supervisor may own unload policy.

### Cited Findings
- **Ollama keep-alive and loading.**
  - Models stay loaded for 5 minutes by default.
  - `keep_alive` accepts a duration, seconds, -1 (forever) or 0 (unload immediately); `OLLAMA_KEEP_ALIVE` sets the server default.
  - `OLLAMA_MAX_LOADED_MODELS` defaults to 3×GPUs (3 on CPU); idle models are unloaded to make room.
  - Default context is 4096.
  - Memory scales with NUM_PARALLEL × context.

  — [Ollama FAQ](https://docs.ollama.com/faq)
- **llama-server fitting and lifecycle flags.**
  - `--fit` is on by default and adjusts unset arguments to fit device memory.
  - `--fit-target` defaults to a 1024 MiB per-device margin, and `--fit-ctx` to a 4096 minimum context.
  - `-ngl` defaults to auto.
  - `--warmup` is enabled by default.
  - `--sleep-idle-seconds` defaults to -1 (disabled).
  - Router mode has `--models-max` (default 4) and `--models-autoload`.
  - `--metrics` exposes a Prometheus endpoint.

  — [manpage](https://manpages.debian.org/unstable/llama.cpp-tools/llama-server.1.en.html); [server README mirror](https://git.mitjafelicijan.com/llmnpc/plain/llama.cpp/tools/server/README.md)
- **Ollama placement.** A model that fits one GPU is placed there; otherwise it is spread across GPUs. `ollama ps` shows the CPU/GPU split. — [Ollama FAQ](https://docs.ollama.com/faq)

### Inferences
- **Auto-fit.** llama.cpp's fitting logic lives in `common` (the `--fit` params). Onyx already links `common`, so it can likely call the same fitting routine before `llama_model_load`, giving "just works" GPU offload and context on any VRAM size. Verify the API name in the pinned llama.cpp revision.
- **Idle unload.** Since JX Runtime owns process lifecycle, Onyx could expose an idle-seconds flag that frees the model and context but keeps the HTTP port, reloading on the next request (llama-server `--sleep-idle-seconds` semantics). Alternatively it can report idle time via `/health` or metrics so JX applies Ollama-like keep-alive.
- **Graceful drain.** On SIGTERM: stop admitting (503), let in-flight slots finish up to a deadline, then exit. No source found for a specific engine's implementation, but it is standard practice.
- **Warmup and metrics.** Warmup (an empty decode at load) removes first-request latency spikes, especially from GPU kernel JIT on Vulkan or Metal. A `/metrics` endpoint (queue depth, cache hit tokens, tok/s) aids JX scheduling.

### Gaps
- Not found: details of Ollama's memory-estimation algorithm, LM Studio's runtime features (JIT loading/TTL), mistral.rs, MLC-LLM, ExLlamaV3 and llamafile specifics, and any benchmark comparing llama-server against those engines. These were not covered within the research budget.
- Graceful-drain behavior of other engines is not sourced.
