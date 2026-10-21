# Onyx-owned speculative decoding on llama.h (without common/speculative or llama-server)

Scope and method. I read the code at Onyx's pin, llama.cpp `9723942adc518b43c4b95dc4dce6906903eb5e09` (b10711), and at master `339f3242c761f16c27e5ffcde1d35dcc31933a55` (2026-10-20). Both were sparse clones, read only and not run. I also read PR pages and a few benchmark write-ups. The code links below use `PIN = https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09` and `MASTER = https://github.com/ggml-org/llama.cpp/blob/master`. Line numbers refer to the pin unless marked otherwise. For Onyx itself I read `src/engine.cpp` (`build_batch` at lines 696–752, `decode_batch` at 754–805) and `src/engine.h` (`onyx_slot` at 160+).

Line counts at the pin, from `wc -l`:

| File | Lines at pin | Lines at master |
|---|---|---|
| `common/speculative.cpp` | 3020 | 3113 |
| `common/speculative.h` | 115 | — |
| `common/ngram-map.cpp` / `.h` | 536 / 115 | — |
| `common/ngram-mod.cpp` / `.h` | 62 / 38 | — |
| `common/ngram-cache.cpp` / `.h` | 285 / 101 | — |
| `common/sampling.cpp` | 919 | 1091 |
| `tools/server/server-context.cpp` | 5531 | 6971 |

The implementations inside `speculative.cpp` break down roughly as follows:

| Implementation | Lines in `speculative.cpp` | Approx. size |
|---|---|---|
| draft-simple | 179–425 | ~245 |
| EAGLE3 | 426–909 | ~485 |
| DFlash/DSpark | 910–1363 | ~455 |
| MTP | 1364–1803 | ~440 |
| ngram-simple | 1804–1848 | ~45, plus ~50 in `ngram-map.cpp` |
| ngram-map-k/k4v | 1849–1902 | ~55, plus ~480 in `ngram-map.cpp` |
| ngram-mod | 1903–2077 | ~175, plus 100 in `ngram-mod.*` |
| ngram-cache | 2078–2216 | ~140, plus 386 in `ngram-cache.*` |
| dispatcher / params / init | 2217–3020 | ~800 |

## 1. The verification loop on llama.h (batch rows, logits, accept/reject, rollback, sampler/grammar/logprobs state)

### Takeaway
Everything the verification loop needs is in the public llama.h. To verify a draft of length k for one slot, add k+1 rows for that `seq_id` with `logits=true`: the last sampled token plus the k draft tokens, at consecutive positions. Read `llama_get_logits_ith` for each row. Then sample-and-compare left to right with the slot's own sampler chain, and roll back the tail with `llama_memory_seq_rm(mem, seq, n_past_accepted, -1)`. For deterministic drafters (n-gram, greedy draft model), "sample from the target, accept iff it equals the draft" already preserves the target distribution exactly at any temperature. The p/q rejection sampling added after the pin (PR #27694) only matters when the drafter itself samples from a distribution q, and upstream measured gains of just +1–6% accepted length. Advance grammar and penalty state with `llama_sampler_accept` only on tokens that are actually accepted. Upstream also keeps a `llama_sampler_clone` snapshot for checkpoint replays.

### Cited Findings
- **Upstream server row layout.** `handle_last_sampled_token()` records `spec_i_batch = [row(sampled), row(d0) … row(dk-1)]` and adds `sampled` at `pos_next()`, then each draft token at `pos0++`. Every row is added with `output=true`. The draft tokens are pushed into the slot's token history optimistically ("prompt.tokens.insert(spec_draft)"). — [PIN tools/server/server-context.cpp L504–542](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L504)
- **Draft length cap.** `get_n_draft_max()` caps the draft at `n_ctx - n_tokens - 2` (leaving one token of headroom for context shift) and at `n_remaining() - 1` of the request's `max_tokens`. — [PIN server-context.cpp L483–502](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L483)
- **Greedy verifier.** `common_sampler_sample_and_accept_n(gsmpl, ctx, idxs, draft)` loops over i < k:
  - `id = common_sampler_sample(gsmpl, ctx, idxs[i])`, then `common_sampler_accept(gsmpl, id, true)`, then push `id`; it breaks on the first `draft[i] != id`.
  - If every draft token matched, it samples one bonus token from `idxs[k]`.
  - The result is accepted drafts plus one correction or bonus token, so every verify step yields at least one token.
  - The code is about 30 lines. — [PIN common/sampling.cpp L678–706](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/sampling.cpp#L678)
- **Rollback after verify.** The server keeps the accepted ids, trims its token mirror (`keep_first(n_tokens - n_draft)` then inserts the accepted ids minus the last), sets `slot.sampled = ids.back()`, and calls `slot.mem.seq_rm(slot.id, pos_next(), -1)`. It then calls `common_speculative_accept(spec, slot.id, accepted.size()-1)` so drafters can update their statistics. — [PIN server-context.cpp L3870–3975](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L3870)
- **Sampler snapshot.** Before verifying, the server takes `smpl_save = common_sampler_clone(slot.smpl)`. It uses the snapshot only on the checkpoint-restore path, where it calls `common_sampler_copy(smpl_save, slot.smpl)`. On the normal partial-accept path, sampler state is correct because accept was only called on emitted tokens. — [PIN server-context.cpp L3884, L3925](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L3884)
- **llama.h primitives Onyx needs, all present at the pin.**
  - `llama_get_logits_ith`: [PIN include/llama.h L1041](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h#L1041).
  - `llama_memory_seq_rm` ("Returns false if a partial sequence cannot be removed"): L748.
  - `llama_sampler_clone` and `llama_sampler_reset`: L1338–1339.
  - `llama_state_seq_get_data_ext` / `set_data_ext` with `LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY`, used for checkpoints: L912–932.
  - Context params `n_outputs_max` and `n_outputs_max_per_seq`: L365–366.

  — [PIN include/llama.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h)
- **Output budget.** The server sizes the output budget as `per_seq = 1 + n_draft` and `total = min(n_batch, n_parallel * (1 + n_draft))`, and writes it into `n_outputs_max` / `n_outputs_max_per_seq`. — [PIN common/speculative.cpp L2629–2638](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L2629); [PIN server-context.cpp L1014–1016](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L1014)
- **Probabilistic verification (PR #27694, post-pin, approved about 2026-10-02).**
  - Mechanism: the drafter samples from q; the target accepts with min(1, p/q); on rejection it draws from the normalized max(0, p−q).
  - The flag is `--spec-draft-sampling greedy|probabilistic`, with greedy as the default. It applies to draft-simple and draft-MTP only. It was not evaluated on EAGLE3, and DFlash did not benefit.
  - Measured gains at temp 0.1–1.0 were about +1% to +6% accepted length. Llama-3.1-8B with a 3B draft on an RTX 5090 gave +6.07% accepted length and +5.21% throughput. MTP on Qwen3.6-35B-A3B gave +2.21%. At temp 0 the outputs are identical by design.
  - N-gram drafters have no q, so they cannot use this path.

  — [PR #27694](https://github.com/ggml-org/llama.cpp/pull/27694)
- **Master implementation of the rejection path.** It is `common_sampler_sample_and_accept_n_rejection(gsmpl, ctx, idxs, draft, draft_q)`, about 100 lines.
  - It applies the grammar mask to the target candidates, rescales the remaining probability mass so the residual is not over-weighted, accepts if `p_x >= q_x || u < p_x/q_x`, and otherwise samples the residual.
  - It uses a separate `std::mt19937 rng` inside the sampler: "independent of the draft, or the target distribution is not preserved".

  — [MASTER common/sampling.cpp L736–840, L125](https://github.com/ggml-org/llama.cpp/blob/master/common/sampling.cpp)
- **Grammar with speculation.** Verification uses the normal `common_sampler_sample` path (grammar applied, `grammar_first` option), and `accept(..., accept_grammar=true)` runs only for emitted tokens. A draft token that violates the grammar therefore never matches and is rejected at that position. The synthetic-acceptance benchmark mode deliberately skips grammar advancement ("synthetic draft tokens do not advance grammar or reasoning state"). — [PIN server-context.cpp L55–95](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L55)
- **Logprobs under speculation.**
  - At the pin, the speculative emit loop has `result.prob = 1.0f; // set later` and `// TODO: set result.probs`, so logprobs were not produced for speculated tokens. — [PIN server-context.cpp L3975–3981](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L3975)
  - At master (after #27196), each accepted token calls `populate_token_probs(slot, result, false, special, spec_i_batch[i])`, reading the pre-sampling logits of that token's verify row. The code comments that "post_sampling_probs is not supported with speculative decoding". — [MASTER server-context.cpp ~L4900–4915](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/server-context.cpp); [PR #27196](https://github.com/ggml-org/llama.cpp/pull/27196)
- **Related post-pin fixes.** Drafts are no longer accepted at EOG ([#29638](https://github.com/ggml-org/llama.cpp/pull/29638)), and n-gram drafts are no longer rejected entirely at temp > 0 ([#29924](https://github.com/ggml-org/llama.cpp/pull/29924)). — (PR list from prior notes `upstream_llamacpp.md`)
- **Onyx today.**
  - `build_batch()` adds exactly one row per GENERATE slot (`common_batch_add(batch, slot.sampled, slot.n_past, {seq}, true)`), then fills the rest of the batch with prompt chunks split evenly across prefilling slots.
  - `decode_batch()` may split the batch into views and halves the view size on `ret == 1`. It samples each slot whose single `i_batch` row fell inside the current view.
  - `compute_token_probs()` already computes logprobs from raw logits at a row index.

  — `/home/user/Onyx-Engine/src/engine.cpp` L696–805, L230–280

### Inferences
- **Onyx's verifier.** About 60–100 lines of own code over `llama_sampler_sample(chain, ctx, idx)` / `llama_sampler_accept`. It mirrors `sample_and_accept_n`, and `onyx_slot` would gain `std::vector<int32_t> i_batch_spec` alongside `i_batch`. For n-gram (the first stage), greedy-match verification is the only correct and needed mode, and it is exact at any temperature. Defer p/q rejection until a model-based drafter exists, since upstream measured only single-digit percentage gains.
- **Sampler state rule.** Call `accept` only on emitted tokens, never on draft tokens. This keeps the grammar, penalties/DRY ring buffers and Onyx's own reasoning-budget state machine consistent without cloning. A clone (`llama_sampler_clone` of the chain) is needed only if Onyx adds checkpoint/replay for recurrent models (section 5). Confirm that the grammar sampler clones correctly before relying on this; I did not verify it in source.
- **Reasoning budget.** Onyx's own state machine (RB_COUNTING / RB_FORCING) must be fed accepted tokens in order. In RB_FORCING the forced tokens are known in advance, so they can be submitted as a draft that is accepted by construction, or speculation can simply be disabled while forcing. Treat EOG and stop strings as truncation points inside an accepted run, and do not emit tokens past them. This is the same class of bug upstream fixed in #29638.
- **Logprobs.** Because Onyx already computes logprobs from raw logits per row, it can call `compute_token_probs(ctx, vocab, spec_row[i], tok, n_probs)` for each accepted token before the next `llama_decode` overwrites the logits. That matches master's pre-sampling semantics.
- **Batch views.** Onyx's `decode_batch` view splitting must never split one slot's k+1 verify rows across two views. Logits from an earlier `llama_decode` are gone after the next call, and recurrent rollback needs the rows in the same ubatch (section 5). Either append spec rows as an atomic group and skip a slot whose group does not fit, or run verification after the whole batch only when it was not split.

### Gaps
- I did not read the full text of PR #27196 (logprobs) or measure the cost of computing full-vocabulary logprobs per verify row.
- I did not check whether `n_outputs_max = 0` (Onyx's default, meaning n_batch) allocates n_batch × n_vocab floats up front at the pin or lazily. If up front, Onyx should set `n_outputs_max` the way upstream does once spec rows exist.

## 2. Composing with continuous batching across slots; how upstream server does it; per-slot draft length; token budget

### Takeaway
Upstream builds one target batch per tick that mixes prefill chunks with, for each generating slot, 1+k_i rows. Each slot gets its own draft length (capped by context, remaining `max_tokens`, `--spec-draft-n-max`, which defaults to 3, and a per-drafter limit). Each slot is verified independently after the shared `llama_decode`. Onyx's tick structure maps onto this directly. The main additions are a draft phase before `build_batch`, a per-slot row vector, and a budget rule so speculative rows do not starve prefill.

### Cited Findings
- **Upstream tick loop (`update_slots`).** — [PIN server-context.cpp L2950–3075](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L2950)
  1. For each GENERATING slot, set `drafting=false`, compute `n_draft_max = slot.get_n_draft_max()`, and if it is positive fill `common_speculative_draft_params{drafting=true, n_max, n_past, id_last=slot.sampled, prompt=&slot.spec_prompt, result=&slot.spec_draft}`.
  2. Call `common_speculative_draft(spec)` once for all slots, so a draft model can batch every slot's draft steps into one draft `llama_decode` per step.
  3. Make a checkpoint where needed (recurrent models, section 5).
  4. Call `handle_last_sampled_token(batch)` for every generating slot.
- **Leftover drafts.** A partial draft left over from a checkpoint replay is reused on the next tick ("we have a previous (partial) draft to reuse"). — same lines
- **Drafter hook after every target decode.** After each successful target `llama_decode` view, the server calls `common_speculative_process(spec, batch_view)`. This lets drafters such as draft-simple, MTP and EAGLE3 consume the same tokens (or hidden states). — [PIN server-context.cpp L3655–3730](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L3655)
- **Defaults.** `n_max = 3`, `n_min = 0`, `p_split = 0.1`, `p_min = 0.0`. For the n-gram drafters: ngram-mod `n_max = 64`, `n_min = 48`; ngram-map/simple `size_n = 12`, `size_m = 48`, `min_hits = 1`. — [PIN common/common.h L326–362](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.h#L326)
- **Default change.** PR #23269 (2026-05-19) changed the default `--spec-draft-n-max` from 16 to 3 and made `--spec-draft-p-min` functional, defaulting to 0.0. A 5090 user saw p-min 0.75 cut average generation from 87 to 77 t/s. — [PR #23269](https://github.com/ggml-org/llama.cpp/pull/23269)
- **Shared n-gram pool.** The ngram-mod hash pool is shared across all server slots, "so one request can benefit from another's text". — [PR #19164](https://github.com/ggml-org/llama.cpp/pull/19164)
- **Multi-slot drafting in draft-simple.** It builds one draft batch containing a row for every drafting `seq_id`, and loops `llama_decode` on the draft context until all sequences stop (n_max reached or `p < p_min`). — [PIN common/speculative.cpp L274–420](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L274)
- **Onyx capacity.** Onyx sizes `n_batch >= max(32, args.n_batch, n_parallel + 1)` and `n_seq_max = n_parallel`, with `kv_unified` false (per-slot KV budget). — `/home/user/Onyx-Engine/src/engine.cpp` L85–89

### Inferences
- **Proposed Onyx tick:**
  - (a) Draft phase: for each GENERATE slot whose spec is enabled, compute `k_i = min(n_max_slot_i, n_ctx_slot - n_past - 2, max_tokens_left - 1, row_budget_i)`.
  - (b) In `build_batch`, add the slot's 1+k_i rows first, before prefill, recording the row indices.
  - (c) Prefill uses the remaining budget, as it does today.
  - (d) After `llama_decode`, verify each slot, truncate its KV and `cache_tokens`, emit the tokens, and set `sampled`/`n_past`.
  - Onyx's `cache_tokens` mirror must be pushed with drafts and trimmed on reject, as upstream does with `prompt.tokens`.
- **Token budget.** Speculation trades prefill throughput for decode latency. Cap total spec rows per tick, for example `sum_i k_i <= n_batch/4` when any slot is prefilling. Shrink k when many slots are generating: verification rows cost little when decode is memory-bound at small batch, but the batch becomes compute-bound as rows grow. Expected benefit falls as `-np` concurrency rises (prior notes `serving_techniques.md`). This is a heuristic, not a measured value.
- **Per-slot drafters.** Because each slot verifies independently, the drafter can be per slot as well, for example n-gram only for slots whose request looks like an edit or RAG task. No other slot is affected by a slot's rejections.

### Gaps
- I found no upstream measurement of how speculation performs at np ≥ 4 with mixed prefill on consumer GPUs.

## 3. N-gram strategies upstream, SuffixDecoding, adaptive on/off heuristics

### Takeaway
Upstream ships five model-free drafters: ngram-simple, ngram-map-k, ngram-map-k4v, ngram-mod and ngram-cache. Each is a few hundred lines or less. The simplest, ngram-simple (a backward linear scan for the last n tokens, then copy the next m), is about 50 lines and is enough for a first version. ngram-mod (a rolling-hash table of n-gram → next token, about 16 MB, shared across slots, with a low-acceptance reset) is the best second step. Upstream's adaptivity is crude: map statistics, a minimum draft length, and a "5 low-acceptance drafts → reset" rule. Onyx should add a real per-slot acceptance EMA kill switch.

### Cited Findings
- **ngram-simple.** `common_ngram_simple_draft(config, tokens, sampled)` is about 50 lines. The pattern is the last `size_ngram - 1` history tokens plus `sampled`. It scans backwards through the history for the most recent earlier occurrence, then returns up to `size_mgram` following tokens. It needs `cur_len > n + m + 1`. It keeps no state and no index, and the scan is O(history × n) per draft. — [PIN common/ngram-map.cpp L49–100](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/ngram-map.cpp#L49)
- **ngram-map-k / k4v.**
  - Data structures: a vector of key n-grams, each with up to `COMMON_NGRAM_MAX_VALUES = 4` candidate m-gram values and their counts and `n_accepted` statistics. An optional hash → index map has `COMMON_NGRAM_HASH_MAP_SIZE = 262144` entries. `min_hits` gates proposals.
  - The map must be rebuilt when the chat history drops a previous reasoning block.
  - `key_only = true` is ngram-map-k; with values it is k4v.
  - It came from PR #18471 (2026-01-28).

  — [PIN common/ngram-map.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/ngram-map.h); [PR #18471](https://github.com/ggml-org/llama.cpp/pull/18471)
- **ngram-mod.**
  - Structure: a hash table `entries[idx(n tokens)] = next token`, with `EMPTY = -1`.
  - `begin()` hashes the whole prompt and resets the table if occupancy exceeds 0.25.
  - `draft_one()` adds new n-grams in chunks of 32, then chains lookups: each found token extends the key window. It stops at EMPTY and discards the draft if it is shorter than `n_min`.
  - `accept()` computes `f_acc = n_accepted / n_draft_last`. If `f_acc < 0.25` five times in a row, it calls `mod.reset()`.
  - It warns if `n_match < 16`.

  — [PIN common/speculative.cpp L1903–2077](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L1903); [PIN common/ngram-mod.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/ngram-mod.h)
- **ngram-mod PR #19164 (merged 2026-01-30).**
  - Design: about 16 MB of constant memory and constant time, one pool shared across slots. The author put the cost at about 5 ms and called it negligible.
  - Results from comments:
    - gpt-oss-120b repeating 200 lines of source: acceptance 0.92 and about 814 tok/s versus about 182 tok/s.
    - Qwen 30B: acceptance 0.05–0.63 across requests.
    - Devstral-2-123B: CRLF line endings gave 0.11 acceptance at 2.3 tok/s, versus LF at 0.83 and about 30 tok/s.
  - The author says it "only helps when the output repeats existing text".
  - The low-acceptance reset fired constantly for one user; raising `--draft-min` fixed it.

  — [PR #19164](https://github.com/ggml-org/llama.cpp/pull/19164)
- **ngram-cache.** The classic lookup decoding, with static and dynamic n-gram caches (`-lcs` / `-lcd`), about 386 lines plus a 140-line implementation. Post-pin, the dynamic cache file is saved (#22055). — [PIN common/ngram-cache.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/ngram-cache.h); prior notes `upstream_llamacpp.md`
- **Combining strategies.** `--spec-type` is a comma list, and drafters are chained: "after the first successful draft from an implementation, we set [drafting] to false". The order is the n-gram types first, then the model-based ones. — [PIN common/speculative.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.h); [PIN speculative.cpp L2655–2669](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L2655)
- **When n-gram hurts.** An HF 128K-context benchmark measured ngram-cache at n=2 with 32% acceptance cutting decode 17% (75 tok/s). — [HF Space](https://victor-mtp-on-hf-endpoints.static.hf.space/index.html) (via prior notes `serving_techniques.md`)
- **SuffixDecoding.**
  - Method: suffix trees over prompts and prior outputs, with adaptive speculation length.
  - On SWE-Bench and OpenHands traces: 1.8–4.5x over vanilla and 1.3–3x over prompt-lookup decoding.
  - In Snowflake ArcticInference production: 1.96–3.12x on BlazeEdit and 1.0–1.28x on SpecBench.

  — [arXiv 2411.04975](https://arxiv.org/html/2411.04975v2); [Snowflake blog](https://www.snowflake.com/en/engineering-blog/suffixdecoding-arctic-inference-vllm/)

### Inferences
- **Stage-1 drafter.** Own ngram-simple-style prompt lookup over `slot.prompt + generated`. Use n = 3–4 for the key (upstream's 12 is conservative), take the most recent match, and draft k ≤ 8–16, which is cheap to verify. An incremental hash map `hash(last n) → last position` makes lookup O(1); this is the "map" variant and about 100–150 lines.
- **Adaptive kill switch** (Onyx-specific; addresses the −17% case):
  - Keep a per-slot EMA of the accepted fraction.
  - Disable the slot's speculation when the EMA is below about 0.3 after 8 or more verify steps, and back off exponentially before probing again.
  - Scale k from the observed run length, SuffixDecoding-style: k = clamp(2 × mean accepted + 1).
  - Draft only when the match length is ≥ n, which removes the empty-proposal overhead.
  - Upstream has no time-based guard. Onyx could additionally compare measured ms/token with and without speculation per slot.
- **Normalization.** The CRLF result shows that drafts depend on exact token sequences. Nothing needs to change, but it explains variance in benchmarks.
- **Suffix tree.** A global suffix tree or automaton over recent outputs (cross-request, SuffixDecoding-style) is stage-1b. It is worthwhile for agent or tool loops where the same JSON or code is regenerated. A suffix automaton is roughly 300–500 lines of C++ (my estimate).

### Gaps
- I found no published per-strategy comparison (simple vs map vs mod vs cache) on the same hardware.

## 4. Draft-model mode: second llama_context on a small model, vocab checks, cost

### Takeaway
This mode uses only llama.h: a second `llama_model` and `llama_context` with `n_seq_max = n_parallel`. The draft context catches up by decoding every target batch with logits off. Drafting runs k sequential batched decodes on the draft context, and the draft KV must be trimmed after verification. The vocab check is about 60 lines of public-API calls. The cost: draft VRAM plus KV for every slot at full `n_ctx`, a doubled prefill, and k small serial decodes per step. On fast GPUs with small targets it can make generation much slower (0.27x measured); it pays off for 14B+ targets and, in one CPU test, for a 3B target.

### Cited Findings
- **Vocab compatibility check, `common_speculative_are_compatible`.** It requires:
  - the same `llama_vocab_type`;
  - the same add_bos/add_eos settings and BOS/EOS ids;
  - an `n_vocab` difference of at most `SPEC_VOCAB_MAX_SIZE_DIFFERENCE`;
  - identical `llama_vocab_get_text(i)` for every id from `SPEC_VOCAB_CHECK_START_TOKEN_ID` up to the smaller vocab.

  — [PIN common/speculative.cpp L67–129](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L67)
- **Draft context setup.** `cparams.n_ctx = llama_n_ctx(ctx_tgt)` ("the draft context holds as many tokens per sequence as the target context"), `n_rs_seq = 0`, `ctx_other = ctx_tgt`. The draft context's `n_seq_max` must equal the spec `n_seq`, otherwise it throws. — [PIN speculative.cpp L2555–2612, L247–250](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L2555)
- **Catch-up and drafting.**
  - `process(batch)` decodes the target's batch on the draft context with `logits = nullptr`.
  - `draft()` seeds `id_last` at `n_past` for every drafting seq, then loops: sample with the draft's top-k sampler, stop that seq if `p < p_min` or n_max is reached, add the token, and decode again.
  - Drafts shorter than `n_min` are cleared.
  - After verification the server calls `llama_memory_seq_rm(ctx_dft_mem, seq, ckpt.pos_max+1, -1)`.

  — [PIN speculative.cpp L262–420](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L262); [PIN server-context.cpp L3036–3040](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L3036)
- **Wasted re-evaluation.** The server notes "TODO: avoid restoring the draft context and re-evaluating the drafted tokens when not needed [TAG_SPEC_AVOID_DRAFT_REEVAL]". The draft KV for drafted tokens is discarded and then re-decoded through `process()`. — [PIN server-context.cpp L3030, L3713](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L3713)
- **Consumer measurements** (InventiveHQ, 2026-06-25; Qwen2.5-Coder targets with a 0.5B draft):

  | Hardware | Target | Result |
  |---|---|---|
  | RTX 5060 Ti, CUDA | 7B | 84.7 tok/s baseline, 0.27x with draft (regressed) |
  | RTX 5060 Ti, Vulkan | 7B | 0.95x |
  | RTX 5060 Ti, CUDA | 14B | 1.40x overall: 1.85x math, 1.57x code, 0.92x chat |
  | CPU only | 3B | 12.9 → 22.1 tok/s (1.72x); chat 1.01x |

  — [InventiveHQ](https://inventivehq.com/blog/llama-cpp-speculative-decoding-consumer-gpu)
- **Other measurements.** The Register (Dec 2024) got Llama 3.1 8B with a draft to about 182 tok/s at 60% acceptance, versus 90–100 tok/s without. — [The Register](https://www.theregister.com/2024/12/15/speculative_decoding/). An 8B + 1B pair gave 1.83x; a poorly matched int4 Gemma 4 draft (41% match) dropped from 60 to 45 tok/s (blog, anecdotal). — [runaihome](https://runaihome.com/blog/speculative-decoding-llama-cpp-local-llm-setup-2026/) (via prior notes)
- **Probabilistic draft sampling on draft-simple** gave +6.07% accepted length and +5.21% throughput (8B + 3B, RTX 5090). — [PR #27694](https://github.com/ggml-org/llama.cpp/pull/27694)

### Inferences
- **Onyx implementation size.** About 300–400 lines: load and check (60), catch-up decode after each target decode (20), batched multi-slot draft loop with a top-k=1 or greedy sampler (120), KV trim (10), plus config and fit. It needs nothing beyond llama.h.
- **Avoiding the re-evaluation.** Onyx can skip upstream's TODO: keep the draft KV for accepted tokens, trim only the rejected tail, then decode only the target's correction or bonus token on the draft context.
- **Verdict.** Worth owning as stage 2, but off by default and gated by an automatic speed probe. It is a poor fit on small or fast targets. It also costs `n_parallel × n_ctx` of draft KV, because Onyx splits KV per slot.

### Gaps
- I found no first-party llama.cpp CPU-only draft-model benchmark; the InventiveHQ CPU number is a single blog result.

## 5. MTP / EAGLE3 / DFlash via llama.h: libllama vs common, downstream drivability, why one slot

### Takeaway
The graphs live inside libllama: the `LLM_GRAPH_TYPE_DECODER_MTP` context type, the hidden-state ("nextn") extraction, `src/models/eagle3.cpp`, and the DFlash models. But the drivers (about 440 lines for MTP, about 485 for EAGLE3, about 455 for DFlash) live in `common/speculative.cpp`. They depend on the staging header `src/llama-ext.h` ("WIP … try as much as possible to not include this header"), not on the public llama.h. A downstream can drive MTP from libllama alone only by including that unstable internal header, which links fine because its symbols are `LLAMA_API`-exported. Expect churn there. The single-slot restriction: at merge (May 2026) MTP refused `n_parallel > 1`. By the pin, the server code has no such check and #23269 fixed multi-slot correctness, but its author says parallel MTP "still remains highly suboptimal" because ubatches cannot use `split_equal()`. EAGLE3 and DFlash were validated only at `-np 1`.

### Cited Findings
- **Public pieces in llama.h.** `enum llama_context_type { DEFAULT, MTP }`, `llama_model_params.load_mtp`, `llama_context_params.ctx_type` and `ctx_other` ("a source/target/parent context … sharing results or llama_memory between 2 contexts"), and `llama_model_n_layer_nextn`. — [PIN include/llama.h L223–226, L349, L370, L418, L593](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h#L223)
- **Pieces only in the staging header.** `llama_set_embeddings_nextn(ctx, value, masked)`, `llama_set_nextn_layer_offset`, `llama_get_embeddings_nextn[_ith]`, `llama_set_embeddings_layer_inp` / `llama_get_embeddings_layer_inp` (EAGLE3/DFlash feature taps), `llama_get_ctx_other`, `llama_model_target_layer_ids[_n]`, `llama_model_dflash_selector_top_k` and `llama_model_get_tok_embd`. The header itself says: "this is a staging header for new llama.cpp API — breaking changes and C++ are allowed. everything here should be considered WIP". `common/speculative.cpp` includes it by relative path: `#include "../src/llama-ext.h" // staging API … (used by MTP)`. — [PIN src/llama-ext.h L1–5, L93–130](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-ext.h); [PIN common/speculative.cpp L13](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L13)
- **The MTP driver.** — [PIN speculative.cpp L1364–1560](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L1364)
  - It creates a second context on the same model with `ctx_type = LLAMA_CONTEXT_TYPE_MTP` and `ctx_other = ctx_tgt`.
  - It sets `llama_set_embeddings_nextn(ctx_tgt, true, masked=false)`, so the target emits pre-norm hidden rows for every token, and `(ctx_dft, true, masked=true)`.
  - It keeps per-seq `pending_h` carryover: hidden state h_p pairs with token x_{p+1} across ubatch boundaries.
  - `process()` re-decodes each target batch on the MTP context, with h rows shifted right by one, and allocates a batch carrying both token and embd: "llama_batch_init allocates only one of token/embd; MTP needs both. TODO: fix, how to call without malloc".
  - It has three modes: Gemma4 shares the target KV; step35 chains n heads; Qwen3.5/3.6 use a single head.
  - It warns "process() hook may not have run on every prefill ubatch (need_embd / logits=1 on every prompt position?)".
  - libllama dispatches `LLAMA_CONTEXT_TYPE_MTP` to `LLM_GRAPH_TYPE_DECODER_MTP` and copies `t_h_nextn` device-to-host per ubatch. — [PIN src/llama-context.cpp L30, L1486–1560, L1947–1962](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-context.cpp#L1486)
- **MTP PR #22673 (merged 2026-05-16).**
  - At merge, loading failed with "MTP currently supports only n_parallel=1".
  - Prefill slowed: on RTX 3090 + 3060 it dropped from 1315 to 665 tok/s, with about 2.49 GiB of extra VRAM.
  - Post-merge TODOs included partial rollback for batch > 1 and multi-sequence recurrent performance.
  - Combining with n-gram drafting was "experimental".

  — [PR #22673](https://github.com/ggml-org/llama.cpp/pull/22673)
- **Later status.** PR #23269 (2026-05-19) "fixes a bug in parallel decoding that uses recurrent rollback when n_rs_seq > 0" and states "parallel decoding with MTP enabled still remains highly suboptimal … we cannot utilize the split_equal() logic". — [PR #23269](https://github.com/ggml-org/llama.cpp/pull/23269). A grep of the pin's `tools/server`, `common` and `src` finds no "n_parallel=1" check; the server calls `common_speculative_init(params.speculative, params.n_parallel)` for every type. — [PIN server-context.cpp L1258–1262](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L1258)
- **Conflicting claim.** The HF 128K benchmark claims MTP "silently disables nParallel > 1 and --cache-reuse". This may describe an earlier build. — [HF Space](https://victor-mtp-on-hf-endpoints.static.hf.space/index.html)
- **Unconfirmed multi-slot bugs.** A third-party note cites issue #27572 and PR #27705 (an MTP async-copy race with several slots that collapses acceptance to 0 on HIP), and a community report describes a Gemma4-MTP crash at `--n-parallel 2`. I did not confirm either on GitHub. — [search summary](https://store.piffa.net/lm/bug/research/deep-research-report_llama_eaman.md)
- **EAGLE3 PR #18039 (merged 2026-06-12).**
  - Code: graph in `src/models/eagle3.cpp`, feature taps in `src/models/llama.cpp`.
  - The draft GGUF holds a one-layer decoder, a feature-fusion layer over three target layers, and a `d2t` draft→target vocabulary map.
  - Results on RTX A6000:

    | Model | Baseline | EAGLE3 | Speedup | Acceptance |
    |---|---|---|---|---|
    | Llama3.1-8B Q4_K_M | 121.5 tok/s | 274.4 tok/s | 2.26x | 92.5% |
    | Llama3.1-8B BF16 | — | — | 3.28x | — |
    | Qwen3-32B Q4_K_M | — | — | 1.24x | 39.7% |

  - On DGX Spark: GPT-OSS-120B gave 0.83–1.08x.
  - Batching more than one sequence is "future work" (examples use `-np 1`), and CUDA graphs are disabled when `n_tokens > 1`.

  — [PR #18039](https://github.com/ggml-org/llama.cpp/pull/18039)
- **DFlash PR #22105 (merged 2026-06-28).**
  - It drafts a whole block in one forward pass.
  - Qwen3.6-27B Q4_K_M on DGX Spark: 2.69x (12.57 → 33.76 t/s), with 2.20x on QA up to 4.07x on RAG.
  - MoE targets: GPT-OSS-20B ranged from 0.61x to 1.27x.
  - Hybrid targets need snapshot, restore and replay.
  - All examples use `-np 1`.

  — [PR #22105](https://github.com/ggml-org/llama.cpp/pull/22105)
- **MTP consumer numbers** (from the #22673 thread):

  | Hardware | Model | Result |
  |---|---|---|
  | RTX 3090 + 3060 | Qwen3.6-27B Q6_K, n=3 | 22.97 → 42.45 tok/s (1.85x), 76% acceptance |
  | RTX 5090 | Qwen3.6-27B q4_0 | about 64 → 105 tok/s |
  | 6 GB laptop 3060 + 64 GB RAM | Qwen3.6-35B-A3B | 22.92 → 29.39 tok/s at n=2, 81.5% acceptance |
  | Strix Halo | Qwen3.6-35B-A3B Q8 | about 40 → 70 tok/s |

  On a Ryzen 7 5825U (Vulkan iGPU), Qwen3.6-35B-A3B Q8 ran at about 12–13 t/s with 0.68–0.71 acceptance (no baseline given; from #23269). — [PR #22673](https://github.com/ggml-org/llama.cpp/pull/22673); [PR #23269](https://github.com/ggml-org/llama.cpp/pull/23269)

### Inferences
- **Why one slot.** This is my reading of the code and PR text, not a statement from upstream. The MTP context must be fed hidden rows for every token of every sequence (unmasked nextn plus a device-to-host copy). It needs per-sequence carryover across ubatches, and with recurrent targets the per-sequence trailing-token constraint conflicts with how mixed multi-sequence batches are split into ubatches. The single-sequence case avoids all of that, so upstream shipped it first and then made multi-slot correct but slow.
- **Onyx options for MTP:**
  - (a) Write its own driver (about 500 lines) against `src/llama-ext.h`, accepting breakage on every pin bump.
  - (b) Vendor upstream's MTP class alone.
  - (c) Wait for the nextn API to move into llama.h.

  I recommend (c), with (a) gated to `np == 1` and Qwen3.6/Gemma4 MTP GGUFs only if the owner wants the 1.6–1.9x single-stream win now. EAGLE3 and DFlash need external draft GGUFs plus the same staging APIs, and are validated only at np = 1, so do not own them yet.

### Gaps
- I could not find the PR that removed the `n_parallel=1` check, or confirm whether issue #27572 and PR #27705 (the multi-slot race) are real.
- I did not measure how much of master's MTP driver diff (pin → master: +93 lines in `speculative.cpp`) touches the staging API.

## 6. Hybrid/recurrent models: rolling back recurrent state after rejected drafts

### Takeaway
Plain recurrent state cannot be partially rolled back: `llama_memory_seq_rm` returns false for a partial range. The pin offers two mechanisms, both reachable from llama.h:
- **Bounded snapshots.** `llama_context_params.n_rs_seq` (EXPERIMENTAL) keeps N per-token state snapshots, so a rollback of up to N tokens succeeds through `seq_rm`, once per decode.
- **Full checkpoints.** Save the sequence state with `llama_state_seq_get_data_ext(..., PARTIAL_ONLY)` before verifying, restore it on reject, then re-decode the accepted prefix. This costs an extra target forward pass per partial rejection.

Upstream probes which case applies by decoding two dummy tokens and trying `seq_rm`.

### Cited Findings
- **`n_rs_seq` parameter.** "number of recurrent-state snapshots per seq for rollback (0 = no rollback) [EXPERIMENTAL]", with the query `llama_n_rs_seq(ctx)`. — [PIN include/llama.h L364, L572](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h#L364)
- **Recurrent memory implementation.**
  - It allocates `mem_size * (1 + n_rs_seq)` rows.
  - Partial rollback via the per-token snapshot index is allowed when `1 <= rollback <= n_rs_seq` and no rollback is already pending ("pending rollback is single-use").
  - Otherwise `seq_rm` returns false.
  - It forces "the trailing (1 + n_rs_seq) tokens of each seq [to] stay in the same ubatch" by calling `split_equal(n_ubatch, true, n_rs_seq + 1)`.

  — [PIN src/llama-memory-recurrent.cpp L101, L193–203, L443–445](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-memory-recurrent.cpp#L193)
- **Capability probe, `common_context_can_seq_rm`.** It clears memory, decodes 2 tokens, and then:
  - returns RS (bounded) if `llama_n_rs_seq(ctx) > 0`;
  - otherwise tries `seq_rm(mem, 0, 1, -1)` and returns FULL (checkpoint needed) on failure, or PART (normal KV) on success;
  - returns NO if decode fails.

  — [PIN common/common.cpp L1583–1623](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.cpp#L1583)
- **Server policy.** It uses a checkpoint if the type is FULL, or if the type is RS and `draft.size() > llama_n_rs_seq(ctx)`.
  - The checkpoint is saved with `ckpt.update_tgt(ctx_tgt, slot.id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY)` before the verify batch.
  - On partial acceptance it loads the checkpoint, calls `seq_rm(pos_max+1, -1)`, truncates the token history, restores the sampler from `smpl_save`, and sets `spec_is_replay = true`. The accepted tokens become the next "draft" and are replayed (re-decoded) without counting as new acceptances.

  — [PIN server-context.cpp L3041–3066, L3896–3928, L3940–3945](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp#L3896)
- **Upstream draft contexts** are created with `n_rs_seq = 0`, with the comment "for small models maybe we can set this to the maximum possible draft". — [PIN speculative.cpp L2573–2576](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/speculative.cpp#L2573)
- **DFlash on hybrid targets** (Qwen3.5) uses snapshot, restore and replay, and "each rejection can cost an extra target forward pass". — [PR #22105](https://github.com/ggml-org/llama.cpp/pull/22105)

### Inferences
- **Onyx on hybrid models** (Qwen3.5/3.6-Next, Granite-H, Nemotron-H and similar):
  - Set `cparams.n_rs_seq = k_max`, for example 4–8, when speculation is enabled and the model is recurrent or hybrid. This multiplies the recurrent state memory by (1 + k_max), which is small relative to KV for SSM layers.
  - Cap k at `n_rs_seq`.
  - Keep each slot's verify rows contiguous and in a single `llama_decode`.
  - Skip the full-checkpoint path at first: with checkpoints, every partial reject costs an extra forward pass, so n-gram speculation with low acceptance would be a net loss on hybrids.
- **Probe.** Port upstream's probe (about 30 lines, llama.h only) and run it at startup to set an `onyx_spec_rollback = {none, bounded, kv}` capability flag.
- **Media prefill.** For mtmd (media) slots Onyx already serializes prefill. Speculation can apply to the generation phase as usual, because rollback only touches positions after the media.

### Gaps
- The `n_rs_seq` path is marked EXPERIMENTAL. I did not find measurements of its memory or speed cost, or confirmation that it works on every backend.

## 7. Measured speedups on consumer GPU/CPU per mode, and when speculation hurts

### Takeaway
- **MTP** gives the most consistent single-stream wins on consumer hardware: about 1.3–1.9x on Qwen3.6 dense and MoE models at 70–80% acceptance. It costs prefill speed (about 2x slower prefill in one test) and VRAM, and multi-slot support is weak.
- **Draft models** range from 0.27x to 1.7x depending heavily on target size and backend.
- **N-gram** ranges from large wins (about 4x when the output repeats the input) to −17% when acceptance is low.
- **EAGLE3 and DFlash** reach 2–3x on dense models but are often neutral or negative on MoE.
- All modes lose below roughly 60–70% acceptance unless drafting is nearly free, and gains shrink with concurrency.

### Cited Findings
- **MTP.**

  | Hardware | Result |
  |---|---|
  | 3090 + 3060 | 1.85x |
  | 5090 | about 1.64x |
  | 6 GB laptop 3060 (MoE, offload) | 1.28x |
  | Strix Halo | about 1.75x |

  Prefill fell from 1315 to 665 tok/s. — [PR #22673](https://github.com/ggml-org/llama.cpp/pull/22673). At 128K context: MTP n=2 gave +5% decode and −22% prefill. — [HF Space](https://victor-mtp-on-hf-endpoints.static.hf.space/index.html)
- **N-gram.** ngram-cache at 32% acceptance: −17% decode. — [HF Space](https://victor-mtp-on-hf-endpoints.static.hf.space/index.html). ngram-mod on repeated code: about 182 → 814 tok/s; on non-repeating text, around 0 acceptance. — [PR #19164](https://github.com/ggml-org/llama.cpp/pull/19164)
- **Draft model.** 5060 Ti 7B/0.5B: 0.27x. 14B/0.5B: 1.40x. CPU 3B/0.5B: 1.72x. — [InventiveHQ](https://inventivehq.com/blog/llama-cpp-speculative-decoding-consumer-gpu)
- **EAGLE3.** A6000 Llama-8B Q4: 2.26x. Qwen3-32B Q4: 1.24x. GPT-OSS-120B: 0.83–1.08x. — [PR #18039](https://github.com/ggml-org/llama.cpp/pull/18039)
- **DFlash.** Spark Qwen3.6-27B: 2.69x. GPT-OSS-20B: 0.61–1.27x. — [PR #22105](https://github.com/ggml-org/llama.cpp/pull/22105)
- **Probabilistic sampling.** +1–6% over greedy at temp > 0. — [PR #27694](https://github.com/ggml-org/llama.cpp/pull/27694)
- **MoE.** Verifying several tokens activates more experts, which lowers the gain. — [PR #18039](https://github.com/ggml-org/llama.cpp/pull/18039); [PR #22105](https://github.com/ggml-org/llama.cpp/pull/22105)

### Inferences
- For Onyx's local single-user case, n-gram covers agent, edit and RAG traffic for free. MTP is the only model-based mode with broad consumer evidence. Draft models need a per-install probe because they regress on fast GPUs.

### Gaps
- There is no first-party CPU-only benchmark for n-gram or MTP. Hardware is unstated in several of the #19164 results.

## 8. Effort estimates and recommended order (verdicts on which modes to own)

### Takeaway
Own the verification core and n-gram drafting now: about 600–900 lines and 1–2 weeks. Own the draft-model mode next as an opt-in: about 400 lines and 1 week. Do not own MTP, EAGLE3, DFlash or DSpark until their hidden-state APIs move from `src/llama-ext.h` into llama.h; at most, offer an `np == 1` experimental MTP. Skip probabilistic rejection sampling until a model drafter exists.

### Cited Findings
- **Upstream sizes for reference:** `speculative.cpp` is 3020 lines (MTP about 440, EAGLE3 about 485, DFlash about 455, draft-simple about 245, n-gram about 400 plus about 1000 in the `ngram-*` files). The verifiers are about 30 lines (greedy) and about 100 lines (rejection). — line counts from the pin and master clones above
- **Complexity signal.** Upstream needed a long run of follow-up fixes for speculation:
  - SWA/recurrent rollback ([#23269](https://github.com/ggml-org/llama.cpp/pull/23269))
  - EOG handling ([#29638](https://github.com/ggml-org/llama.cpp/pull/29638))
  - logprobs ([#27196](https://github.com/ggml-org/llama.cpp/pull/27196))
  - n-gram drafts at temp > 0 ([#29924](https://github.com/ggml-org/llama.cpp/pull/29924))
  - mtmd ([#30257](https://github.com/ggml-org/llama.cpp/pull/30257), [#28715](https://github.com/ggml-org/llama.cpp/pull/28715))
  - slot save ([#30574](https://github.com/ggml-org/llama.cpp/pull/30574))

  — prior notes `upstream_llamacpp.md`

### Inferences
Staged plan. The effort estimates are mine; the line numbers are for Onyx's own code.

1. **Stage 0, plumbing: about 2–3 days, 200–300 lines.**
   - Per-slot `spec_rows`.
   - Atomic row groups in `build_batch`, never split across `decode_batch` views.
   - The greedy verifier over `llama_sampler_sample` / `llama_sampler_accept`.
   - KV and `cache_tokens` trimming.
   - Truncation at EOG and stop strings.
   - Per-accepted-token logprobs via the existing `compute_token_probs`.
   - Metrics: drafted, accepted, accepted per position.
   - The startup rollback-capability probe.
   - A test harness asserting bit-identical output at temp 0 with speculation on and off (the strongest correctness check).
2. **Stage 1, n-gram prompt lookup: about 3–5 days, 200–400 lines.**
   - A per-slot incremental hash index over prompt plus output.
   - Adaptive k and the acceptance-EMA kill switch.
   - A spec-row budget against prefill.
   - Default on, with a request-level opt-out.
   - Optional stage 1b: a cross-request suffix automaton (SuffixDecoding-style) for agent loops, about 1 week.
3. **Stage 2, hybrid models: about 2–3 days.**
   - `n_rs_seq = k_max` for recurrent and hybrid models, with k capped.
   - The checkpoint/replay fallback only if measurements justify it.
4. **Stage 3, draft model: about 1–1.5 weeks, about 400 lines.**
   - A second context, the vocab check, the batched draft loop, and draft-KV trimming without upstream's re-evaluation.
   - A startup or periodic A/B speed probe that auto-disables it.
   - Memory fit for `np × n_ctx` of draft KV.
5. **Stage 4, MTP: about 2–3 weeks, about 600 lines plus ongoing maintenance.**
   - Only behind an `--experimental-mtp` flag with np == 1, written against `src/llama-ext.h`.
   - Otherwise wait for a public nextn API. Track [#9289](https://github.com/ggml-org/llama.cpp/issues/9289) for llama.h changes.
6. **Do not own:** EAGLE3, DFlash, DSpark (external heads, staging API, np = 1 validation only, MoE regressions), probabilistic verification (+1–6%), and synthetic-acceptance benchmarking.

### Gaps
- The effort numbers are engineering estimates and are not sourced.
- I found no upstream statement on when the `llama-ext.h` nextn API will be promoted to llama.h.
