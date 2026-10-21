# KV cache state in Onyx using only llama.h: host-RAM prompt cache, context checkpoints, slot save/restore, unified KV

Scope and method. I read the upstream code directly, not the PR pages. Sources:
- llama.cpp at the Onyx pin `9723942adc518b43c4b95dc4dce6906903eb5e09` (b10711, called PIN below).
- llama.cpp master at `339f3242c761f16c27e5ffcde1d35dcc31933a55` (2026-10-20, called MASTER below).
- Both were sparse clones in the scratchpad; no upstream code was built or run.
- Onyx sources under `/home/user/Onyx-Engine/src/`.

The GitHub API/GraphQL was blocked in this session, so PR descriptions come from the squash-merge commit messages in `git log`, which carry the PR text. Line numbers refer to the PIN tree unless marked MASTER.

URL prefixes used below:
- PIN = `https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09`
- MASTER = `https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55`

## 1. How llama-server's host-RAM prompt cache works (data structures, save/match/evict, limits, -np) and which llama.h calls it uses

### Takeaway
The upstream RAM prompt cache is small and copyable.
- **Structure.** It is a `std::list` of `{token list, checkpoints, full per-sequence state blob}`.
- **Save point.** Saving happens only at slot-selection time, when a slot is about to be reused for a different prompt. It is triggered when the chosen slot would keep less than half its context (f_keep < 0.5), or when the slot was picked by LRU.
- **Restore point.** Right after saving, the server restores the cached entry that beats the slot's own content on both f_keep and f_sim.
- **Eviction.** Oldest entry first (FIFO by insertion, effectively LRU because restored entries are removed from the list), bounded by both bytes and tokens.
- **llama.h calls.** Only `llama_state_seq_get_size_ext` / `get_data_ext` / `set_data_ext` with flags `NONE`/0 plus `llama_memory_seq_rm`. Onyx can reproduce it in about 200–300 lines.

### Cited Findings
**Data structures** ([server-task.h L566-L637](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-task.h)):
- `server_prompt { server_tokens tokens; std::list<common_prompt_checkpoint> checkpoints; }`
- `server_prompt_data { std::vector<uint8_t> main, drft; }`, where `drft` holds the draft-model context.
- `server_prompt_cache_state { prompt; data; }`. Its `size()` counts the blob plus all checkpoint blobs.
- `server_prompt_cache { std::list<server_prompt_cache_state> states; size_t limit_size /*bytes, 0=no limit*/; size_t limit_tokens; alloc(); load(); update(); }`
- A checkpoint is `common_prompt_checkpoint { int64_t n_tokens; int id_task; llama_pos pos_min, pos_max; std::vector<uint8_t> data_tgt, data_dft, data_spec; }` ([common.h L1283+](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/common.h)).

**Construction** ([server-context.cpp L1351-L1360](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)):
- Built as `prompt_cache = std::make_unique<server_prompt_cache>(params_base.cache_ram_mib, n_ctx)`, so the token limit defaults to the context size.
- Defaults in [common.h L628-L632](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.h): `cache_ram_mib = 8192` (-1 = no limit, 0 = disabled), `cache_idle_slots = true`, `n_ctx_checkpoints = 32`, `checkpoint_min_step = 8192`, `kv_unified_per_slot = 0`.

**Save** (`server_slot::prompt_save`, [server-context.cpp L299-L323](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)):
1. `n = llama_state_seq_get_size_ext(ctx_tgt, slot_id, LLAMA_STATE_SEQ_FLAGS_NONE)`, plus the draft context if any.
2. `prompt_cache.alloc(prompt, n_tgt, n_dft)`.
3. `llama_state_seq_get_data_ext(ctx_tgt, buf, n, slot_id, LLAMA_STATE_SEQ_FLAGS_NONE)`.

The slot's checkpoint list is copied into the entry alongside the blob ([server-task.cpp L1711-L1791](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-task.cpp)).

**`alloc` policy** ([server-task.cpp L1711-L1791](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-task.cpp)):
- (a) If an existing entry already contains the whole new prompt as a prefix, skip.
- (b) If the entry is larger than `limit_size`, skip.
- (c) Erase every cached entry that is a prefix of the new prompt (it is now obsolete).
- (d) `pop_front()` oldest entries until it fits.
- (e) Allocate inside try/catch. On `bad_alloc`, the cache shrinks `limit_size` to 40% of its current size and gives up.

**`load` / match** ([server-task.cpp L1793-L1868](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-task.cpp)):
- The baseline is the slot's current content, with `f_keep = lcp/slot_tokens` and `f_sim = lcp/new_tokens`.
- For each cached entry it computes `f_keep_cur = lcp/entry_tokens` and `f_sim_cur = lcp/new_tokens`, skipping entries with `f_keep_cur < 0.25` ("don't trash large prompts"). It picks the entry that improves *both* f_keep and f_sim.
- It restores with `llama_state_seq_set_data_ext(ctx_tgt, data, size, id_slot, 0)` and checks that the return value equals the size.
- It then frees the blob, moves `prompt` (tokens + checkpoints) into the slot, and **erases the entry from the cache** (move semantics: an entry lives either in a slot or in RAM, never both).
- Matching is plain linear LCP over tokens. There is no hashing or radix tree.

**`update` / eviction** ([server-task.cpp L1870-L1901](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-task.cpp)):
- Pops the oldest entry while over the byte limit.
- Estimates bytes per token and raises the token limit to `limit_size/size_per_token` when that is larger.
- Pops the oldest entry while over the token limit.

**When it runs** (`get_available_slot`, [server-context.cpp L1540-L1645](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)):
1. Slot pinned by `id_slot`, otherwise the idle slot with the best LCP similarity `f_sim > slot_prompt_similarity`. The default is 0.1 ([common.h L694](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.h)).
2. If the chosen slot would keep `f_keep < 0.5` of its tokens, `update_cache = true`.
3. If no slot matched, take the LRU slot and set `update_cache = true`.
4. If `update_cache`: `prompt_save()` the slot, then `prompt_load()` the best entry for the new task; if the load fails, `prompt_clear()` (`seq_rm(id,-1,-1)`). Then `prompt_cache->update()`.

**`--cache-idle-slots`** ([server-context.cpp L2408-L2424](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp); init L1420-L1433):
- After a task is launched, every idle slot is saved to the cache.
- With `kv_unified` the idle slot is also cleared, which frees pool cells. Without unified KV the copy is published but its VRAM KV stays.

**`-np` interaction:**
- The cache is one global list shared by all slots.
- The blob is seq-id agnostic. `llama_kv_cache::state_read_meta` reads the stored seq id, "directly discard[s] it", and uses `dest_seq_id` ([llama-kv-cache.cpp L2376-L2420](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-kv-cache.cpp)). An entry saved from slot 2 can therefore be restored into slot 0.
- Restore first calls `seq_rm(dest_seq_id,-1,-1)` and then `find_slot` for the cells (non-contiguous allowed). It fails if the sequence's share of the KV cache lacks room.

**Post-pin fixes:**
- Save only restorable states (stateless embedding/rerank slots excluded; decision models opt-in only): [#30498](https://github.com/ggml-org/llama.cpp/pull/30498), commit 2639a3b4e, 2026-10-15.
- Prefer empty slots, and have child tasks save the state they overwrite ("prompt cache reads go from 5 to 36" in their 4-request test): [#30567](https://github.com/ggml-org/llama.cpp/pull/30567), commit b0d239164.
- RAM limit enforcement: [#25070](https://github.com/ggml-org/llama.cpp/pull/25070) (pre-pin, per earlier notes).

**Origins:**
- The feature came from [#16391](https://github.com/ggml-org/llama.cpp/pull/16391), merged 2025-10-09 (d00cbea63): "server : initial host-memory prompt caching … cache prompts and checkpoints only for completion tasks … add -cram CLI arg".
- PR #16391 originally said it was incompatible with mtmd because `server_tokens` wasn't copyable ([earlier notes](/home/user/Onyx-Engine/research_notes/Onyx%20Engine%20improvement%20opportunities/serving_techniques.md)). At PIN, `server_prompt::clone()` calls `tokens.clone()` ([server-task.h L580-L585](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-task.h)), so that restriction no longer holds structurally.

**Onyx today:**
- Each slot keeps `cache_tokens`.
- Admission picks the idle slot with the longest LCP, otherwise LRU ([engine.cpp L606-L627](/home/user/Onyx-Engine/src/engine.cpp)).
- It then calls `llama_memory_seq_rm(mem, seq, prefix, -1)` and falls back to full clear if that fails or `seq_pos_min > 0` (L667-L682).
- Onyx calls no `llama_state_*` function.

### Inferences
- A port to Onyx is a straight translation. The engine thread owns everything, so no locking is needed.
- Store `std::vector<llama_token>` plus the checkpoint list plus `std::vector<uint8_t>` per entry. Do save-then-load at admission time, inside `admit()` before the `seq_rm` trim.
- Keep the "move, not copy" semantics. The restored entry is erased, so RAM never holds a duplicate of VRAM state.
- The f_keep ≥ 0.25 and "improves both f_keep and f_sim" rules are heuristics worth copying verbatim; they come from upstream tuning.
- Onyx's FIFO admission picks a slot before checking the RAM cache. The natural integration is: choose a slot (empty first, then LRU, as in #30567), save its state if it is worth keeping, then restore the best RAM entry into it.

### Gaps
- No upstream benchmark of hit rates or TTFT for `--cache-ram` was found. The PR has none; a third-party figure (11.7k-token prompt, 9.9 s → 0.97 s) is in the earlier notes.

## 2. How context checkpoints work for SWA and recurrent/hybrid memory, and which fixes landed after the pin

### Takeaway
A checkpoint is a `LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY` snapshot. It holds only the part of the sequence's memory that `seq_rm` cannot roll back: the SWA window cells, and the recurrent state.
- **When taken.** Right before a prefill `llama_decode`, at chosen batch boundaries: user-message starts, plus 4+n_ubatch and 4 tokens before the prompt end. At most 32 per slot, spaced at least 8192 tokens apart.
- **Restore.** On the next request, if live memory can't serve the shared prefix (`pos_min ≥ threshold`), the newest checkpoint whose `pos_min` is below the threshold is restored. `n_past` is clamped to that checkpoint, `seq_rm(pos_next,-1)` drops the rest of the full-attention KV, and only the tail is re-prefilled.
- **Post-pin fixes.** Several correctness fixes landed after the pin (#28302, #30514, #30580, #30666, #26004, #27530). An Onyx port should copy the MASTER logic, not the PIN logic.

### Cited Findings
**Flags** ([llama.h L906-L918](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h)):
- `LLAMA_STATE_SEQ_FLAGS_NONE 0`
- `LLAMA_STATE_SEQ_FLAGS_SWA_ONLY 1`, kept for backwards compatibility
- `LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY 1`: "work only with partial states, such as SWA KV cache or recurrent cache (e.g. Mamba)"
- `LLAMA_STATE_SEQ_FLAGS_ON_DEVICE 2`

**What PARTIAL_ONLY saves at PIN:**
- **iSWA cache:** skips `kv_base`, writes `kv_swa` ([llama-kv-cache-iswa.cpp L259-L273](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-kv-cache-iswa.cpp)).
- **Hybrid** (attention + recurrent): skips `mem_attn`, writes `mem_recr` ([llama-memory-hybrid.cpp L190-L202](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-memory-hybrid.cpp)).
- **Plain `llama_kv_cache::state_write`:** ignores the flag (`GGML_UNUSED(flags)`) and writes all non-SWA-masked cells of the seq ([llama-kv-cache.cpp L2094-L2162](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-kv-cache.cpp)).
- **Masking in general:** cells masked by SWA relative to `seq_pos_max` are never written (`is_masked_swa`, same function), so even a NONE save of an SWA cache holds only the window.

**Post-pin change to PARTIAL_ONLY semantics** ([#30514](https://github.com/ggml-org/llama.cpp/pull/30514), e467e5b5e, 2026-10-15, "kv-cache: honor PARTIAL_ONLY in the plain KV cache"):
- The plain cache now writes and reads nothing for PARTIAL_ONLY unless it is SWA (`if ((flags & PARTIAL_ONLY) && swa_type == LLAMA_SWA_TYPE_NONE) return;`).
- The wrappers (iswa, hybrid, hybrid-idx, dsa-iswa) now just delegate, because hybrid wrappers had "also dropped an SWA window that is partial state".
- Motivation: a dense draft/MTP context "copied its whole KV into each server checkpoint".
- At PIN, a PARTIAL_ONLY get on a dense model returns the *full* KV. Onyx should therefore never take checkpoints on dense models (upstream gates on that too).

**Which contexts get checkpoints** ([server-context.cpp L3433-L3446](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)):
- `do_checkpoint = n_ctx_checkpoints > 0 && task is completion && (seq_rm_type == FULL || seq_rm_type == RS || n_swa > 0)`.
- `n_swa = swa_full ? 0 : llama_model_n_swa(model)` (L1204).
- The seq_rm type is probed once at startup by `common_context_can_seq_rm` ([common.cpp L1583-L1623](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.cpp)):
  1. `llama_memory_clear`, then decode 2 tokens with `llama_batch_get_one`.
  2. If `llama_n_rs_seq(ctx) > 0`, the type is RS (bounded recurrent rollback).
  3. Else if `!llama_memory_seq_rm(mem, 0, 1, -1)`, the type is FULL (no partial removal: recurrent/hybrid).
  4. Otherwise PART.
  5. Clear again and synchronize.
- Every call in that probe is llama.h, so Onyx can copy it.

**When checkpoints are taken** (prefill loop, [server-context.cpp L3520-L3608](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)):
- The batch fill loop breaks early at user-message starts, using `task.params.message_spans` from chat-template `message_delimiters`, [server-context.cpp L4277-L4290](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp) and `common/chat.cpp`. It breaks at the last user message, or at a user message more than `checkpoint_min_step` past the last checkpoint.
- It also breaks so that exactly `4 + n_ubatch` and `4` tokens remain ("make 2 checkpoints near the end of the prompt", [#20288](https://github.com/ggml-org/llama.cpp/pull/20288), 2026-03-10).
- Mid-prompt checkpoints are skipped unless the batch starts a user message or is near the end of the prompt.
- No checkpoint is taken when `pos_min < 0` or after an mtmd chunk in the same batch.
- The checkpoint is created **before** `llama_decode` of the batch, so it reflects the state through `n_tokens_start`. Its `n_tokens` is `slot.prompt.n_tokens() - n_tokens_cur`.
- `pos_min`/`pos_max` come from `llama_memory_seq_pos_min/max` at that moment.

**`create_checkpoint`** ([server-context.cpp L2297-L2345](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)):
- Evicts checkpoints within `checkpoint_min_step` of an earlier one, except those from the current task. Evicts the oldest while at or over `n_ctx_checkpoints`.
- Saves with `llama_state_seq_get_size_ext/get_data_ext(…, PARTIAL_ONLY)` (via [common.cpp L2283-L2317](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.cpp)). A size mismatch is `GGML_ABORT`.
- An upstream TODO, `[TAG_CHECKPOINTS_FIX_POS_MIN]`, admits the recorded `[pos_min,pos_max]` range is "incorrectly determine[d]… not true for SWA models".
- Post-pin [#28302](https://github.com/ggml-org/llama.cpp/pull/28302) (5d806aa25, 2026-09-08) fixed a related problem: the spacing eviction "drops the checkpoint at n_tokens - 4 that the next request resumes from" for prompts shorter than min_step. Spacing eviction is now applied only when the list is full, and an existing checkpoint at the same n_tokens is replaced.

**Restore** ([server-context.cpp L3190-L3375](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)):
1. `n_past = LCP(slot.tokens, new)`.
2. `pos_next = pos at n_past`. `has_new_tokens = n_past < n_tokens`.
3. Threshold at PIN: `pos_min_thold = max(0, pos_next - n_swa - (has_new_tokens?0:1))`.
4. If `n_past > 0`, read `pos_min = llama_memory_seq_pos_min(mem, slot)`. `-1` here aborts as "should not happen".
5. **If `pos_min >= pos_min_thold`** (live memory no longer covers the window needed to continue from `n_past`): search checkpoints newest-first for one with `pos_max <= pos_next && (pos_min < thold || pos_min == 0)`.
6. If found: `llama_state_seq_set_data_ext(ctx, ckpt.data, size, slot, PARTIAL_ONLY)`, then `pos_next = min(pos_next, max(ckpt.pos_min+1, ckpt.pos_max))` and `n_past = min(tokens_up_to(pos_next), ckpt.n_tokens)`.
7. If none is found: full reset (`n_past = 0`), logged as "forcing full prompt re-processing … SWA or hybrid/recurrent memory".
8. Erase checkpoints with `pos_max > pos_next`.
9. Ensure at least one token is evaluated (`n_past--` if equal, `[TAG_PROMPT_LOGITS]`).
10. `keep_first(n_past)`.
11. Before batching, `seq_rm(slot, p0 = pos_next(), -1)` (L3411-L3415). This trims the full-attention KV (or the hybrid's attention part) back to the restored point; the partial restore has already set the SWA/recurrent part.

**Post-pin threshold fix** ([#30580](https://github.com/ggml-org/llama.cpp/pull/30580), 61ad80bf9, 2026-10-15):
- New code: `pos_eval = pos_next - (has_new_tokens?0:1); pos_min_thold = max(0, n_swa > 0 ? pos_eval - n_swa + 2 : pos_eval)`.
- Commit text: "a SWA state starting at pos - n_swa + 1 holds the whole window, but the reuse threshold asked for two more cells… a state read back from a slot file or from the RAM prompt cache was always rejected and the slot rolled back to a checkpoint." ([MASTER server-context.cpp L4057-L4067](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/tools/server/server-context.cpp))

**Post-pin invalidation fix** ([#30666](https://github.com/ggml-org/llama.cpp/pull/30666), f174553e1, 2026-10-19, "fix swa checkpoint invalidation logic"):
- Records `n_prefix = n_past` before any restore, and also erases checkpoints with `cur.n_tokens > n_prefix`: ones that "cover tokens not shared with the new prompt" ([MASTER L4172](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/tools/server/server-context.cpp)).
- PIN's `pos_max > pos_next` test alone could keep a checkpoint built over a divergent token history.

**Other post-pin fixes:**
- Failed restores used to leave half-written K/V or recurrent state. [#27530](https://github.com/ggml-org/llama.cpp/pull/27530) (08618ff8e, 2026-09-26) "fix K/V and recurrent state cleanup after failed restores".
- At PIN, `state_read_sinfo` already does `seq_rm(seq_id,-1,-1)` and throws on failure ([llama-kv-cache.cpp L2164-L2242](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-kv-cache.cpp)).
- Checkpoints are preserved across slot save/restore: [#26004](https://github.com/ggml-org/llama.cpp/pull/26004) (033df86b6, 2026-10-08).

**Recurrent specifics:**
- `llama_memory_recurrent::seq_rm` fails a partial removal at the end unless it falls within `n_rs_seq` per-token snapshots. Comment: "models like Mamba or RWKV can't have a state partially erased… because their state isn't preserved for previous tokens" ([llama-memory-recurrent.cpp L161-L210](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-memory-recurrent.cpp)).
- `n_rs_seq` is an `[EXPERIMENTAL]` context param, clamped to 0 for unsupported archs ([llama.h L364](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h); [llama-context.cpp L104-L109](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-context.cpp)).

**Onyx today:**
- [engine.cpp L113-L116](/home/user/Onyx-Engine/src/engine.cpp) sets `cache_reuse_ = 0` for `n_swa > 0 || !can_shift`, which disables **all** LCP reuse.
- Upstream disables only `n_cache_reuse` (KV-shift chunk reuse) when `!can_shift`, and keeps LCP prefix reuse plus checkpoints for those models ([server-context.cpp L1185-L1195](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp) vs. L3190-L3375).

### Inferences
- **Onyx's gate is broader than upstream's.** Onyx's `cache_reuse_` serves both as the LCP-reuse enable and the minimum prefix. Upstream's `n_cache_reuse` is the separate shift-based chunk reuse. Without checkpoints, a correct Onyx gate would:
  - keep LCP reuse on SWA models, using the threshold check `pos_min < pos_min_thold` instead of the current `pos_min > 0` test at [engine.cpp L672](/home/user/Onyx-Engine/src/engine.cpp);
  - on recurrent/hybrid, allow reuse only when `seq_rm(prefix,-1)` succeeds. That happens when the new prompt extends the cached tokens exactly, i.e. `prefix == cache_tokens.size()` and the cached tokens all reached the KV.

  The current `pos_min > 0` check rejects every valid SWA state once the conversation is longer than the window.
- **Multi-turn chat diverges at the end.** The template usually re-renders the previous assistant turn, for example stripping reasoning, so the exact-extension case is rare. That is why upstream takes the two checkpoints just before the end of the prompt (#20288) and at user-message starts.
- **Message boundaries.** Onyx renders its own chat templates. To find user-message starts it needs its own delimiter list (upstream gets these from `common/chat.cpp`'s per-template `message_delimiters`). A simpler first version can use only the "end of prompt − 4 − n_ubatch" and "end − 4" checkpoints.
- **Implementation recipe for Onyx (llama.h only):**
  1. At startup, probe the seq_rm type (as in `common_context_can_seq_rm`) and `n_swa = llama_model_n_swa(model)`. Enable checkpoints only for FULL/RS/`n_swa > 0`.
  2. In `build_batch`, cap each prefilling slot's chunk so it ends at `n_prompt - 4 - n_ubatch` and at `n_prompt - 4`.
  3. Before the `llama_decode` that will cross that boundary, take a checkpoint: `pos_min/max = llama_memory_seq_pos_min/max`, `sz = llama_state_seq_get_size_ext(ctx, seq, PARTIAL_ONLY)`, then `llama_state_seq_get_data_ext(…)`. Onyx packs several slots per decode; the call only touches that seq's state, but must run between decodes.
  4. At admission: compute `n_past = LCP`, `pos_eval`, `thold` (MASTER formula); read `pos_min`.
  5. If `pos_min >= thold`, find the newest checkpoint with `pos_max <= pos_next && n_tokens <= n_prefix && (pos_min < thold || pos_min == 0)`.
  6. Restore it with `set_data_ext(…, PARTIAL_ONLY)` and clamp `n_past`.
  7. Erase invalid checkpoints, `n_past = min(n_past, n_prompt-1)`, and `llama_memory_seq_rm(mem, seq, n_past, -1)`.
- **Pitfall.** For hybrid memory, `seq_rm(p0,-1)` on the attention part works, but the recurrent part rejects partial removal. Check whether `llama_memory_seq_rm` returns false after a successful partial restore; if it does, the hybrid wrapper fails as a whole. I did not trace hybrid `seq_rm` return semantics, so this needs a test.

### Gaps
- I did not find upstream measurements of how often checkpoints hit in practice.
- I did not verify how `llama_memory_hybrid::seq_rm` combines the attention and recurrent return values after a partial restore. Upstream relies on `slot.mem.seq_rm`, a common wrapper that aborts on failure ([common.cpp L1625-L1630](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.cpp)), which implies success in that path.

## 3. Memory and time cost of state blobs (dense vs SWA vs recurrent) and ON_DEVICE semantics

### Takeaway
- **Dense full-attention:** a full-state blob scales linearly with tokens, and checkpoints are pointless.
- **SWA:** a checkpoint is bounded by the window. Note the SWA cache size is `n_swa + n_ubatch` cells per sequence.
- **Recurrent:** the state is a fixed size per sequence, independent of length.
- **ON_DEVICE:** keeps the bytes in one device-side buffer per seq_id. It is fast but holds only one snapshot per seq, and upstream server/common use it nowhere. Onyx should use host blobs.

### Cited Findings
**Blob layout:**
- A seq blob is: magic `0xaf143cd8`, seq_id, then the memory module's `state_write` ([llama-context.cpp L3050-L3090](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-context.cpp)).
- The KV part, per stream: cell_count; per-cell pos / n_seq_id / ext / seq ids; then per layer the K type, K row size and K rows, then V (or transposed V) ([llama-kv-cache.cpp L2094-L2300](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-kv-cache.cpp)).
- Restore validates the layer count, K type, row size and V transposition, and checks that `cell_count <= cells.size()` (state_read_data, L2533-L2580).

**SWA cache size:** `size_swa = GGML_PAD(min(size_base, n_swa*(unified ? n_seq_max : 1) + n_ubatch), 256)` ([llama-kv-cache-iswa.cpp L73](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-kv-cache-iswa.cpp)).

**`swa_full` caveat:** llama.h warns "setting to false when n_seq_max > 1 can cause bad performance in some cases" ([llama.h L403-L405](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h)).

**ON_DEVICE** ([llama.h L914-L916](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h)):
- Doc: "Keeps the tensor data on device buffers (i.e. not accessible in host memory, but faster save/load). Getting the state for a seq_id with this flag invalidates all prior states gotten for that seq_id with this flag."
- Implementation: `llama_io_write_device` copies into `mem_storage[seq_id]`, a per-seq device buffer that is reallocated when the layout changes ([llama-context.cpp L2729-L2790, L3060-L3110](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-context.cpp)).
- The recurrent cache aborts on ON_DEVICE with more than one cell range ([llama-memory-recurrent.cpp L824-L826](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-memory-recurrent.cpp)).
- `grep ON_DEVICE` over `tools/server` and `common` at both PIN and MASTER finds no users. The server's checkpoints and prompt cache use host blobs only.

**Upstream logging:** the server logs each checkpoint and cache entry size in MiB ("created context checkpoint … size = %.3f MiB", [server-context.cpp L2341-L2344](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)). No published size tables were found.

### Inferences
Worked numbers from the KV formula `bytes/token = 2 × n_layers_attn × n_kv_heads × head_dim × bytes_per_elem`. The model hyperparameters come from my own knowledge and were not re-verified this session.
- **Llama-3.1-8B, f16 KV** (32 layers × 8 KV heads × 128): 128 KiB per token. A full NONE blob for a 32k-token conversation is about 4 GiB, and 8k tokens is about 1 GiB. With the default 8 GiB `--cache-ram`, that is only a handful of long conversations. A q8_0 KV roughly halves this, since blobs keep the cache's type.
- **gpt-oss-20b** (24 layers, half of them SWA with a 128-token window, 8 KV heads × 64):
  - The full-attention part is about 24 KiB per token.
  - An SWA checkpoint is about 12 layers × ~128 cells × 2 × 512 × 2 B ≈ 3 MiB.
  - Thirty-two checkpoints per slot is about 100 MiB.
- **Recurrent / hybrid** (Mamba2, Qwen3-Next/3.5-style linear attention): the recurrent state per sequence is constant, about d_inner×d_state×layers plus the conv state. Each checkpoint costs the same, typically tens of MiB.
- **Time.** Get and set are device↔host copies plus small metadata parsing, so they are roughly bounded by PCIe bandwidth: about 40 ms per GiB at ~25 GB/s. This is far cheaper than re-prefilling the same tokens.
- **Blocking.** The copy blocks the engine thread, so for Onyx all decoding pauses during a large save. Save only on eviction, as upstream does, never per token.
- **ON_DEVICE.** Its single-snapshot-per-seq rule rules it out for a multi-checkpoint list or a RAM cache. It may suit a one-off "speculative rollback" snapshot, which Onyx does not need yet.

### Gaps
- There are no measured blob sizes or save/restore timings for Gemma 4, Qwen3.5 hybrid or Granite 4. I could not run models here.
- Exact recurrent state sizes per architecture were not computed from GGUF metadata.

## 4. Unified KV (kv_unified=true): per-slot caps upstream (--kv-unified-per-slot, PR #24124), admission/preemption changes, and whether it is worth it for Onyx

### Takeaway
With `kv_unified=true` all sequences share one cell pool. `llama_n_ctx_seq` then reports the whole pool. Upstream's per-slot cap is purely a server-side limit on `slot.n_ctx`.
- **Upstream on a full pool:** clears idle slots one at a time (after saving them to the RAM cache), then halves the batch. It never preempts active slots.
- **For Onyx:** worthwhile once the RAM prompt cache exists, because idle slots can then be spilled instead of pinning VRAM. Without the cache, unified KV mostly adds failure modes.

### Cited Findings
- `kv_unified`: "use a unified buffer across the input sequences when computing the attention / try to disable when n_seq_max > 1 for improved performance when the sequences do not share a large prefix" ([llama.h L406-L408](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h)).
- [#24124](https://github.com/ggml-org/llama.cpp/pull/24124) (18443257a, 2026-08-27) "server: add ctx-per-slot (--kv-unified-per-slot)".
  - The cap is applied only in `n_ctx_slot() = min(llama_n_ctx_seq(ctx), kv_unified_per_slot, n_ctx_train)` ([server-context.cpp L3999-L4008](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)).
  - At startup, if the cap exceeds the pool it warns: "cap has no effect… unset -c to size it to n_parallel * kv_unified_per_slot" (L1210-L1238).
- **Decode failure for lack of KV space** ([server-context.cpp L3700-L3707](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)): `if (!try_clear_idle_slots()) n_batch /= 2;` then retry.
- `try_clear_idle_slots` (L1656-L1681) only acts when `kv_unified`, purging "slots one by one" with a TODO: "move slot to level 2 cache instead of removing? … try to store and resume later?"
- With `cache_idle_slots`, idle slots are saved to RAM and cleared on each new task in unified mode (L2408-L2424).
- **Onyx today:**
  - Sets `n_seq_max = n_parallel` and leaves `kv_unified` false, so `n_ctx_slot_ = llama_n_ctx_seq(ctx_)` is `n_ctx/np` ([engine.cpp L86-L111](/home/user/Onyx-Engine/src/engine.cpp)).
  - Treats `llama_decode` ret==1 as a terminal "context size has been exceeded" error and clears the slot ([engine.cpp L770-L790](/home/user/Onyx-Engine/src/engine.cpp)).
- A post-pin auto-fit change for unified KV, [#28849](https://github.com/ggml-org/llama.cpp/pull/28849), was reverted in [#29437](https://github.com/ggml-org/llama.cpp/pull/29437) (from [earlier notes](/home/user/Onyx-Engine/research_notes/Onyx%20Engine%20improvement%20opportunities/upstream_llamacpp.md)).

### Inferences
Changes Onyx would need for unified KV:
1. **Context params:** set `cparams.kv_unified = true`, with the cap as `n_ctx_slot_ = min(llama_n_ctx_seq, per_slot_cap, n_ctx_train)`.
2. **Admission:** move from "an idle slot exists" to "an idle slot exists AND (estimated) free cells ≥ prompt + reserve". llama.h exposes no free-cell count; I found no such API in the PIN `llama.h` memory section. So Onyx must track its own accounting (sum of `cache_tokens.size()` over live seqs, plus SWA overhead) or rely on decode failure.
3. **Decode ret==1:** make it retriable. First spill idle slots to the RAM cache and `seq_rm` them, then halve the chunk. Only then fail or preempt the youngest generating slot. Preemption can be recompute-style (requeue prompt + generated tokens) or swap-style (save to the RAM cache).
4. **Prefix sharing:** with unified KV, `llama_memory_seq_cp(mem, src, dst, 0, n)` can share a common prefix across slots without duplicating cells, for example for `n>1` choices. Upstream uses this for child tasks.

The trade-off: the llama.h comment says non-unified is faster when sequences don't share prefixes. For Onyx's typical small `-np` (2–4) with independent chats, the win is flexibility (one long request can use the whole pool), not throughput. Recommend: implement after the RAM cache, behind a flag, defaulting off.

### Gaps
- I did not read #24124's PR discussion (API blocked), only the commit and code.
- There are no measurements of unified vs non-unified attention cost at small `-np`.

## 5. Slot save/restore to disk via llama_state_seq_save_file: file format, version bump, safety

### Takeaway
`llama_state_seq_save_file` writes:
- magic `GGSQ`
- version `LLAMA_STATE_SEQ_VERSION`: 3 at PIN, 4 on MASTER after #28498
- `u32` token count, then the tokens
- the seq blob (without the in-memory magic/seq header)

Loading rejects a version mismatch, so files are not portable across a pin bump. Validation is structural only: layer count, types and row sizes. A file from a different model of the same shape loads silently. Upstream adds a filename whitelist, a token-validity check and (post-pin) a checkpoint appendix. Onyx should treat slot files as trusted local cache, not user uploads.

### Cited Findings
- **File format at PIN:** `state_seq_save_file` writes `LLAMA_STATE_SEQ_MAGIC`, `LLAMA_STATE_SEQ_VERSION`, `u32 n_token_count`, raw tokens, then `state_seq_write_data(io, seq_id, 0)` (flags = 0, i.e. a full state) ([llama-context.cpp L3230-L3250](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-context.cpp)).
- **`state_seq_load_file`:**
  - Rejects a magic or version mismatch.
  - With `tokens_out == nullptr` it only reports the count, bounded by file size: the two-call pattern of probing the count, then loading.
  - Rejects a count above capacity, then restores with flags 0. It asserts on trailing-size consistency (L3185-L3228).
  - Upstream uses the two-call pattern in [server-context.cpp L2585-L2600](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp).
- **Version bump:** `LLAMA_STATE_SEQ_VERSION` 3→4 and `LLAMA_SESSION_VERSION` 10→11 in [#28498](https://github.com/ggml-org/llama.cpp/pull/28498) (210791069, 2026-10-05), "kv-cache: save exact KV rotation metadata, reject restoring mismatched rotation".
- **No model identity check:** `state_seq_read_data` just calls `memory->state_read`. It has no arch or model check, unlike the full-context `state_read_data`, which checks the arch string ([llama-context.cpp L3311-L3317](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-context.cpp), L3253-L3290).
- **Upstream server safety:**
  - Requires `--slot-save-path` and rejects filenames failing `fs_validate_filename` ([MASTER server-context.cpp L4746, L5267-L5307](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/tools/server/server-context.cpp)).
  - On restore it validates that the restored size fits `slot->n_ctx` and that `restored.validate(ctx)` holds (token ids in vocab), clearing the slot on any failure ([PIN server-context.cpp L2600-L2620](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)).
  - At PIN the token payload is `server_tokens::serialize()`, a packed format carrying media chunks since [#26640](https://github.com/ggml-org/llama.cpp/pull/26640). So upstream's files are not plain token arrays when media is present.
- **Post-pin additions:**
  - A checkpoint appendix after the llama payload with magic `SCKP` (`0x504b4353`). Blob sizes are bounded by the bytes left; it "fall[s] back to full prompt re-processing when a checkpoint restored from a slot file fails to load" and keeps the last `n_ctx_checkpoints` ([#26004](https://github.com/ggml-org/llama.cpp/pull/26004); [MASTER L2735-L2830](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/tools/server/server-context.cpp)).
  - Draft state is saved too ([#30574](https://github.com/ggml-org/llama.cpp/pull/30574)).
- **SWA files restored at PIN were always rejected** by the off-by-two threshold until [#30580](https://github.com/ggml-org/llama.cpp/pull/30580) (commit text quoted in §2).

### Inferences
**Onyx design recommendations:**
- **(a) Own container format.** Don't rely on `llama_state_seq_save_file` alone. Write an Onyx header: magic, Onyx format version, `LLAMA_STATE_SEQ_VERSION`, a model fingerprint (GGUF file hash or size+mtime+arch+n_layer+KV types), n_ctx_seq and flags. Follow it with the tokens, the full blob from `llama_state_seq_get_data_ext(…, NONE)` and the checkpoint blobs (PARTIAL_ONLY), each length-prefixed. Restore with `set_data_ext` (NONE for the main blob, then PARTIAL_ONLY per checkpoint, kept in RAM only until needed).
  - This gives the checkpoint persistence upstream added post-pin, plus a model check llama.h lacks.
  - Alternatively, write `llama_state_seq_save_file` and a sidecar file.
- **(b) Path safety.** Restrict files to a configured directory, allow only a simple filename charset, and enforce a size cap before allocation.
- **(c) Token checks.** After restore, verify every token is `< llama_vocab_n_tokens` and that the count is `≤ n_ctx_slot`.
- **(d) On version mismatch,** treat the file as a cache miss, not an error.
- **Exposure.** Untrusted files could crash the process: `GGML_ASSERT`s in the load path, and abort on size mismatch in the checkpoint helpers. So slot load should not be exposed to remote users without auth.

### Gaps
- I did not fuzz or read every `state_read_data` bounds check. Whether malformed blobs can trigger out-of-bounds reads, as opposed to throws or asserts, is unverified.

## 6. Multimodal interaction: why Onyx disables reuse with mtmd, and whether checkpoints and the prompt cache can work with media chunks

### Takeaway
Media can work with LCP reuse and the RAM cache if Onyx keeps a token list that marks media spans with a content hash and tracks position counts (M-RoPE images use fewer positions than tokens). Upstream does exactly this:
- `LLAMA_TOKEN_NULL` placeholders plus chunk ids, where the id is the sha256 of the media bytes;
- `pos_next` / `size_up_to_pos` for position accounting.

Upstream refuses only KV-shift chunk reuse with media, and never takes a checkpoint in a batch that contained a media chunk. Onyx's blanket disable is broader than upstream's.

### Cited Findings
- **Onyx disables both `cache_reuse_` and context shift when `--mmproj` is loaded.** The code comment says "a media chunk occupies KV positions that cannot be prefix-matched token-by-token nor partially discarded… (mirrors llama-server's load-time force-disable)" ([engine.cpp L118-L140](/home/user/Onyx-Engine/src/engine.cpp)).
- **Onyx media prefill:** `prefill_media` clears the seq and evaluates all chunks via `mtmd_helper_eval_chunk_single`, serializing the engine loop ([engine.cpp L816-L890](/home/user/Onyx-Engine/src/engine.cpp)).
- **What upstream's disable covers:** it disables `n_cache_reuse` (the shift-based reuse) with mtmd ([server-context.cpp L1178-L1182](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)). The prefix path asserts `!has_mtmd` only inside the shift branch (L3203-L3216). LCP via `get_common_prefix` treats two media chunks as equal when `mtmd_input_chunk_get_id` and the token counts match, then skips the chunk ([server-common.cpp L680-L727](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-common.cpp)).
- **Media ids:** `mtmd_helper_bitmap_init_from_buf` sets the bitmap id to `hash_sha256_hex(buf, len)`, "use sha256 to prevent cache poisoning" ([mtmd-helper.cpp L385-L426](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/mtmd/mtmd-helper.cpp)). Onyx already uses this helper (engine.cpp L842).
- **Position accounting:** `server_tokens::pos_next()` adds `mtmd_input_chunk_get_n_pos - n_tokens` per media chunk ([server-common.cpp L372-L400](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-common.cpp)). That is why the checkpoint code uses `pos_next` and `size_up_to_pos` rather than token indices.
- **Checkpoints with media:** "do not checkpoint after mtmd chunks" (`do_checkpoint = do_checkpoint && !has_mtmd`, [server-context.cpp L3594-L3595](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/server/server-context.cpp)).
- **Slot files with media** are supported via the packed token serialization ([#26640](https://github.com/ggml-org/llama.cpp/pull/26640), 2026-08-12).

### Inferences
**Onyx path:**
1. Represent the slot's cache as a vector of entries: `{token}`, or `{media: sha256 id, n_tokens, n_pos}`.
2. Compute LCP with chunk-level equality.
3. Track `pos` separately from the token index.
4. Pass `n_past` in positions to `mtmd_helper_eval_chunk_single` for the remaining chunks only, instead of clearing the seq.

For SWA or recurrent vision models (Gemma 3/4 are SWA), checkpoints must sit only at text-only batch boundaries. This works naturally if the end-of-prompt checkpoints fall after the last image. The RAM cache blob is media-agnostic: it is just KV. Only the token list needs the media ids.

Effort: medium. Most of the work is the token/position bookkeeping and partial-chunk evaluation, not llama.h calls.

### Gaps
- I did not check whether `mtmd_helper_eval_chunk_single` with non-causal image attention (Gemma) places any constraint on resuming mid-sequence beyond correct `n_past`.

## 7. Effort estimate for an Onyx-owned version, main correctness risks, and test strategy with tiny models

### Takeaway
Everything needed is in llama.h. The total is about 1,000–1,500 new lines of Onyx C++, roughly 2–3 engineer-weeks including tests, if done in this order:
1. Fix the reuse gate and threshold.
2. Checkpoints.
3. RAM prompt cache.
4. Slot save/restore.
5. Media-aware LCP.
6. Unified KV.

The main risks are silent wrong-output bugs from position and threshold off-by-ones, and stale checkpoints. Upstream had exactly these bugs after the pin. Test by comparing cached-path and cold-path logits on tiny dense, SWA and recurrent models.

### Cited Findings
- **Upstream's post-pin correctness bugs** are the risk catalogue:
  - off-by-two SWA threshold ([#30580](https://github.com/ggml-org/llama.cpp/pull/30580));
  - stale checkpoints covering divergent tokens ([#30666](https://github.com/ggml-org/llama.cpp/pull/30666));
  - min-step eviction dropping the resume checkpoint ([#28302](https://github.com/ggml-org/llama.cpp/pull/28302));
  - PARTIAL_ONLY copying a full dense KV ([#30514](https://github.com/ggml-org/llama.cpp/pull/30514));
  - failed restore leaving garbage ([#27530](https://github.com/ggml-org/llama.cpp/pull/27530));
  - mismatched KV rotation on restore ([#28498](https://github.com/ggml-org/llama.cpp/pull/28498));
  - non-restorable states filling the cache ([#30498](https://github.com/ggml-org/llama.cpp/pull/30498)).
- **Upstream tests to mirror:**
  - `tools/server/tests/unit/test_slot_save.py`. #30580 added SWA cases and "override_kv, used to give the SWA test model a window smaller than the test context".
  - `test_completion.py`. #30666 added a test "that fails with > n_past, but works with > n_prefix".
  - `tests/test-save-load-state.cpp`, a matrix over models and KV types (#28498).
  - `test-llama-context`, consolidated in [#30023](https://github.com/ggml-org/llama.cpp/pull/30023). #30514 added "partial state is not larger than the full state and, without SWA, does not grow with the tokens".

### Inferences
**Effort breakdown** (my estimate):
- Gate/threshold fix plus seq_rm-type probe: about 50 LOC, 0.5 day.
- Checkpoints (struct, take in `build_batch`, restore at admit, invalidation, eviction): about 300 LOC, 3–4 days.
- RAM prompt cache (list, alloc/load/update, admission integration, `--cache-ram` flag, `/metrics` counters): about 300 LOC, 2–3 days.
- Slot save/restore endpoints with Onyx container format, path checks and checkpoints: about 250 LOC, 2 days.
- Media-aware token list, LCP and partial chunk eval: about 300 LOC, 3–4 days.
- Unified KV with retry/spill/preempt: about 200–300 LOC, 3–5 days, plus tuning.

How much of upstream to reproduce: the cache heuristics (f_keep/f_sim rules), the checkpoint placement rules, the MASTER threshold and invalidation formulas, and the save-on-slot-reuse trigger. Skip the draft/spec checkpoint data, aLoRA, child tasks, decision models and `n_cache_reuse` shift reuse.

**Correctness risks:**
- (1) Checkpoint `[pos_min,pos_max]` semantics on SWA (upstream's own TODO `[TAG_CHECKPOINTS_FIX_POS_MIN]`).
- (2) Restoring into a slot whose sequence lacks room in non-unified mode: `set_data` returns 0, so clear and re-prefill.
- (3) Onyx's batched decode. Checkpoint get/set must happen between `llama_decode` calls, never while other slots' rows for that ubatch are pending. Because Onyx splits a batch into views (engine.cpp L760-L790), take checkpoints before the whole batch.
- (4) `cache_tokens` must exactly mirror the KV. Onyx pushes the sampled token (L716) and prompt tokens (L748). The last sampled token is not decoded at stop, so verify the cache tokens never include an un-decoded token. Otherwise LCP claims a position that has no KV.
- (5) Context shift (`seq_add`) changes positions. Saved checkpoints become invalid after a shift, so clear them.
- (6) Memory blow-up: 32 checkpoints × slots × RAM-cache entries. Count checkpoint bytes in the cache limit, as upstream does.

**Test strategy:**
- Tiny models: a stories15M-class dense llama GGUF; a tiny SWA model, either a small Gemma-3-270M or a dense tiny model with a GGUF KV override forcing a small `n_swa`, as upstream's test harness does; and a tiny Mamba/RWKV or hybrid (e.g. a small Granite-4-H or Mamba-130M GGUF).
- Core oracle: for random multi-turn prompts, greedy-decode N tokens with (a) cold full prefill and (b) the cached/restored/checkpoint path. Assert identical tokens and near-identical top-k logits.
- Unit tests:
  - Partial blob size is constant in tokens for recurrent, and bounded by the window for SWA.
  - The restore→seq_rm→prefill sequence equals cold output.
  - Invalidation when the prompt diverges before a checkpoint.
  - Save→file→restart→load round trip, and a version-mismatch file treated as a miss.
  - Eviction under a tiny `--cache-ram`.
  - Two slots swapping conversations through the RAM cache (cross-seq restore).
- Run each test at `-np 1` and `-np 3`, and with q8_0 KV.

### Gaps
- No tiny SWA or hybrid GGUFs were located or verified in this session. The upstream test suite's model list (`tools/server/tests/utils.py`) was not read in detail.
- The effort numbers are my judgement, not measured.
