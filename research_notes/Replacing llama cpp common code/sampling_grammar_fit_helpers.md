# Replacing llama.cpp common/: sampling wrapper, JSON-schema→grammar, tokenize/batch/log helpers, memory auto-fit, MoE offload

How the sources were read: the pin `9723942adc518b43c4b95dc4dce6906903eb5e09` (b10711, committed 2026-08-30) and master `339f3242c761f16c27e5ffcde1d35dcc31933a55` (2026-10-20) were cloned and read locally. Master is **1072 commits** ahead of the pin (`git rev-list --count`). Link conventions below:
- PIN = `https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/`
- MASTER = `https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/`

Line counts come from `wc -l`. Onyx usage was taken from `/home/user/Onyx-Engine/src/*.cpp` (grep counts are given inline).

**What Onyx actually calls from common/ (outside the chat layer):**
- `engine.cpp`:
  - `common_sampler_init`, `common_sampler_sample`, `common_sampler_accept`, `common_sampler_free`
  - `common_tokenize` ×1, `common_detokenize` ×1, `common_token_to_piece` ×3
  - `common_batch_add` ×3, `common_batch_clear` ×2
- `server.cpp`:
  - `common_sampler_init`/`free`, used only to validate a grammar
  - `common_params_sampling`, `common_grammar`, `common_grammar_trigger`
  - `json_schema_to_grammar`, `common_tokenize`
  - **`common_json` ×160**, from common/json.h
- `main.cpp`: `common_log_set_verbosity_thold(-1)`

Onyx does not use any common `LOG_*` macro. It also does not use `common_sampler_get_candidates`, clone/copy, `sample_and_accept_n`, the reasoning-budget sampler (Onyx has its own "v2" state machine, engine.cpp ~L922), or backend sampling. Logprobs are computed by Onyx itself from `llama_get_logits_ith` (engine.cpp `compute_token_probs`).

---

## 1. common_sampler: what does it add over the llama.h sampler chain, how small can an Onyx wrapper be, and are all the samplers in libllama?

### Takeaway
**Verdict: REPLACE.** `common_sampler` is a thin orchestration layer: it builds a chain, adds grammar rejection sampling and logit-bias/suppress tokens, and prefills the grammar. Every actual sampler is in libllama, and the penalty/DRY token history lives inside the libllama samplers, not in common. The Onyx subset is about 150–200 LOC on llama.h alone. Effort is about 1 day plus tests. Risk is low to medium; the risky parts are the grammar fast path and lazy-trigger conversion, both of which must be copied exactly.

### Cited Findings
- `common/sampling.cpp` is 919 lines at the pin and 1091 on master; `common/sampling.h` is 125/134 lines — [PIN common/sampling.cpp](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/sampling.cpp), [MASTER common/sampling.cpp](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/sampling.cpp)
- The header describes what common_sampler adds: "grammar support, custom sampler logic based on the parameters, history of the last accepted tokens, performance metrics". It also explains the speed trick: "the grammar can be applied only to the sampled token in order to verify if it fits the grammar. And only if the token doesn't fit the grammar, the grammar constraints are applied to the full vocabulary and the token is resampled." — [PIN common/sampling.h L10-L31](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/sampling.h#L10-L31)
- `struct common_sampler` holds `params`, `grmr`, `rbudget`, `chain`, a `ring_buffer<llama_token> prev`, `cur`/`cur_p` candidate storage and timing. `set_logits()` copies either backend-sampled probs/logits (`llama_get_sampled_probs_ith` / `llama_get_sampled_logits_ith` / `llama_get_sampled_candidates_ith`) or the full-vocab `llama_get_logits_ith` into a `llama_token_data_array` — [PIN common/sampling.cpp L110-L165](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/sampling.cpp#L110-L165)
- `common_sampler_init` (pin L188-L470) does the following:
  - Validates that the penalties are finite and throws on bad values.
  - Builds the grammar sampler with `llama_sampler_init_grammar`, or with `llama_sampler_init_grammar_lazy_patterns` from triggers. WORD triggers are converted via `regex_escape`, PATTERN_FULL triggers are anchored with `^…$`, and TOKEN triggers become token lists. It throws "failed to parse grammar" when the grammar sampler cannot be built.
  - Tokenizes `generation_prompt` and feeds those tokens to the grammar for OUTPUT_FORMAT/TOOL_CALLS grammars (`common_grammar_needs_prefill`).
  - Merges user `logit_bias` with `llama_vocab_get_suppress_tokens` (-INF).
  - Builds the chain from `params.samplers` (penalties, dry, top_n_sigma, top_k, typical, top_p, min_p, xtc, temp_ext, infill, adaptive_p), ending with `dist`. If mirostat is set, it builds temp + mirostat v1/v2 instead.
  - Disables backend sampling when a grammar or reasoning budget is present.
  - Source: [PIN common/sampling.cpp L188-L470](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/sampling.cpp#L188-L470)
- `common_sampler_sample` (pin ~L583-L665) works in these steps:
  1. Call `llama_synchronize` and `set_logits`.
  2. Return early if a backend sampler already chose a token (`llama_get_sampled_token_ith`).
  3. Apply the reasoning budget, then the chain.
  4. Check the chosen token alone against the grammar, using a 1-element `llama_token_data_array`.
  5. Only if that token is invalid, rebuild logits, apply the grammar to the full vocabulary, and resample.
  6. `grammar_first=true` forces the slow path.
  - Source: [PIN common/sampling.cpp](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/sampling.cpp#L583)
- `common_sampler_accept` feeds the token to rbudget (if any), to the grammar (only if `is_generated` and the grammar is active), and to the chain, then pushes it into `prev` — [PIN common/sampling.cpp ~L470-L510](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/sampling.cpp#L470)
- Token history for penalties is **inside libllama**:
  - `struct llama_sampler_penalties` holds its own `ring_buffer<llama_token> prev` and a `token_count` map — [PIN src/llama-sampler.cpp L2856-L2866](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-sampler.cpp#L2856-L2866)
  - DRY keeps `ring_buffer<llama_token> last_tokens` — [PIN src/llama-sampler.cpp ~L3326](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-sampler.cpp#L3326)
  - Common's own `prev` ring buffer only serves `common_sampler_last`/`prev_str`, which Onyx never calls.
- The pin's llama.h exports all of these samplers:
  - `llama_sampler_init_greedy`, `dist`, `top_k`, `top_p`, `min_p`, `typical`, `temp`, `temp_ext`, `xtc`, `top_n_sigma`, `mirostat`, `mirostat_v2`
  - `grammar`, `grammar_lazy` (deprecated), `grammar_lazy_patterns`
  - `penalties`, `dry`, `adaptive_p`, `logit_bias`, `infill`
  - Source: [PIN include/llama.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h) (grep of `llama_sampler_init_*`)
- Backend (on-GPU) sampling is configured through `llama_context_params.samplers` (`llama_sampler_seq_config`, L352/L413), `llama_get_sampled_token_ith` (L1075) and `llama_set_sampler` (L1331) — [PIN include/llama.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h#L352). It was added in b7628 ([#17004](https://github.com/ggml-org/llama.cpp/pull/17004)) per the [libllama API changelog #9289](https://github.com/ggml-org/llama.cpp/issues/9289). Common disables it whenever a grammar is used.
- Master change worth copying, from [#29797](https://github.com/ggml-org/llama.cpp/pull/29797) (2026-10-07, "sampling : use greedy selection for eligible temperature-zero chains"): when `n_probs==0` and the chain ends in temp==0 (with dynatemp 0) or top_k==1, common now adds `llama_sampler_init_greedy()` instead of `dist` — [MASTER common/sampling.cpp](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/sampling.cpp) (seen via `git show d0b490f2`)
- Other changes to common/sampling between pin and master: probabilistic draft sampling ([#27694](https://github.com/ggml-org/llama.cpp/pull/27694)), `common_sampler_sample_and_accept_n_rejection`, and the new helpers `common_sampling_add_preserved_tokens` / `common_sampling_add_grammar_triggers`. The last two duplicate what Onyx's own `apply_grammar` in server.cpp L398-L430 already does — [MASTER common/sampling.h](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/sampling.h)
- `common_params_sampling` defaults (pin common.h L224-L299) set the order and initial values Onyx inherits today:
  - Chain order: penalties, dry, top_n_sigma, top_k, typical_p, top_p, min_p, xtc, temperature.
  - Defaults: top_k=40, top_p=0.95, min_p=0.05, temp=0.80, penalty_last_n=64, dry_multiplier=0 (disabled).
  - Source: [PIN common/common.h L224-L299](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.h#L224-L299)

### Inferences
- **Minimum Onyx wrapper**, about 150–200 LOC, needs these parts:
  - An `onyx_sampling_params` struct with only the fields `parse_sampling`/`apply_grammar` set: seed, temp, top_p, top_k, min_p, the three penalties, last_n, grammar string + type + lazy, triggers, preserved_tokens, generation_prompt.
  - A chain builder:
    - logit_bias from suppress tokens
    - penalties (skip when disabled)
    - top_k, top_p, min_p (skip the identity samplers to save per-token work)
    - temp_ext
    - greedy-or-dist, per #29797
  - The grammar builder (lazy patterns, plus the ~10-LOC regex_escape for WORD triggers) and the prefill of generation_prompt for non-USER grammars.
  - `sample()` with the single-token grammar check and full-vocab fallback, which is about 40 LOC.
  - `accept()` and `free()`.
  - It uses only llama.h: `llama_sampler_chain_init/add/apply/accept/free/reset`, `llama_sampler_apply` on a 1-element array, `llama_get_logits_ith`, `llama_vocab_n_tokens`, `llama_vocab_get_suppress_tokens`, `llama_sampler_init_*`.
- Since Onyx already computes full-vocab logprobs itself, it can reuse one `std::vector<llama_token_data>` per slot to avoid reallocating, as common does. With a single engine thread this keeps per-token CPU cost the same as common's.
- Correctness traps to replicate:
  - Grammar accept only on generated tokens. Onyx's forced reasoning tokens currently call `accept(tok, false)`, which skips the grammar.
  - Feed prefill tokens only to non-USER grammars.
  - Keep the leading-space skip on the first prefill token (pin sampling.cpp, prefill loop).
  - Throw on a null grammar sampler.
  - Keep sampler construction on the calling thread (engine.cpp L347-L357 relies on exceptions).
- Port the existing upstream test ideas (`tests/test-sampling.cpp`, `test-grammar-integration.cpp`) as Onyx tests for the rejection path.
- Speculative/MTP decoding and backend sampling are the main things a lean wrapper would give up. If Onyx later wants either, porting `sample_and_accept_n` is about 40 more LOC. Backend sampling needs per-seq `llama_set_sampler` wiring and is incompatible with grammars anyway.

### Gaps
- I did not benchmark the rejection-sampling fast path against grammar-first on Onyx workloads.
- I did not confirm whether `llama_sampler_init_grammar_lazy` (deprecated) will be removed on master soon.

---

## 2. json_schema_to_grammar: size, coverage, cost of owning it, alternatives (llguidance, XGrammar), and whether GBNF speed matters for Onyx

### Takeaway
**Verdict: KEEP upstream's converter, don't rewrite it. Vendor a copy only if common/ is dropped entirely.**
- It is 1264 LOC at the pin. Master rewrote it into a schema IR (json-schema.cpp 514 + json-schema-to-grammar.cpp 1028 + headers).
- It covers a large slice of JSON Schema, has about 1650 lines of upstream tests, and the chat/tool-call layer uses it heavily (33 call sites in common/chat*.cpp).
- A from-scratch Onyx version would take weeks to reach parity and would almost certainly be worse.
- llguidance is faster but needs a Rust toolchain. XGrammar is C++ but brings its own submodules (dlpack, picojson, cpptrace) and a different grammar engine, which goes against "few dependencies".
- GBNF slowness is real for pathological grammars, but common's rejection-sampling path already hides most of it for typical JSON output.

### Cited Findings
- Sizes:
  - Pin: `common/json-schema-to-grammar.cpp` 1264 lines, `.h` 43 — [PIN](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/json-schema-to-grammar.cpp)
  - Master: `.cpp` 1028, `.h` 23, plus new `common/json-schema.cpp` 514 and `json-schema.h` 198 — [MASTER](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/json-schema-to-grammar.cpp)
  - Pin to master diff: +604/−… lines (`git diff --stat`, 604 changed lines).
- Pin API:
  - `json_schema_to_grammar(const common_json&, bool force_gbnf)`, `common_schema_info`, `common_grammar_builder{add_rule, add_schema, resolve_refs}`, `build_grammar()`, `gbnf_format_literal()`
  - It depends on `json.h` (common_json) and `common.h`, and uses `<regex>`.
  - Source: [PIN json-schema-to-grammar.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/json-schema-to-grammar.h)
- Master API change: `add_schema` now takes a `common_chat_schema`, and a new overload takes a `common_chat_schema_document`. `common_schema_info` was removed. This came from "common : implement common_schema internal representation for JSON schemas" ([#28736](https://github.com/ggml-org/llama.cpp/pull/28736)) and "chat : refactor API" ([#30210](https://github.com/ggml-org/llama.cpp/pull/30210)), seen via `git log` pin..master. Other fixes in the window: empty object schemas ([#28279](https://github.com/ggml-org/llama.cpp/pull/28279)) and escaped hyphen in regex patterns ([#29127](https://github.com/ggml-org/llama.cpp/pull/29127)).
- Keywords handled at the pin (string-literal grep):
  - `$ref`, `allOf`, `anyOf`, `oneOf`, `const`, `enum`, `pattern` (regex→GBNF)
  - `minLength`/`maxLength`, `minItems`/`maxItems`, `items`, `prefixItems`
  - `minimum`/`maximum`/`exclusiveMinimum`/`exclusiveMaximum` (integer range rules), `properties`, `required`, `additionalProperties`, `format`
  - Built-in `PRIMITIVE_RULES` and `STRING_FORMAT_RULES` (date, time, date-time, uuid, …)
  - Source: [PIN json-schema-to-grammar.cpp L16, L234-L265](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/json-schema-to-grammar.cpp#L234)
- Upstream test coverage: `tests/test-json-schema-to-grammar.cpp` 1653 lines and `tests/test-grammar-integration.cpp` 1493 lines — [PIN tests/](https://github.com/ggml-org/llama.cpp/tree/9723942adc518b43c4b95dc4dce6906903eb5e09/tests)
- The converter is also the grammar backbone of the chat layer: 33 references to `json_schema_to_grammar`/`build_grammar` in `common/chat*.cpp` at the pin (grep).
- The GBNF engine itself (`src/llama-grammar.cpp`, 1525 lines at the pin) lives in libllama and stays — [PIN src/llama-grammar.cpp](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-grammar.cpp)
- GBNF performance:
  - Upstream's grammar guide says "Grammars currently have performance gotchas (see issue #4218)". It also warns that `x? x? x?…` repetition "may result in extremely slow sampling"; use `x{0,N}` instead — [grammars/README.md](https://raw.github.com/ggerganov/llama.cpp/master/./grammars/README.md), [PIN grammars/README.md L125](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/grammars/README.md#L125)
  - The tracked issue "llama : speed-up grammar sampling" collected reports of significant, sometimes exponential, slowdowns with complex or deeply nested grammars — [Weekly GitHub report, Dec 2024](https://buttondown.com/weekly-project-news/archive/weekly-github-report-for-llamacpp-2024-12-02/). This is an aggregator; the primary source is [#4218](https://github.com/ggml-org/llama.cpp/issues/4218).
- llguidance:
  - Built with `-DLLAMA_LLGUIDANCE=ON`; it "requires the Rust compiler, which complicates the llama.cpp build process".
  - It claims ~50μs average mask computation (p99 0.5ms, p100 20ms) for a 128k tokenizer on JSON Schema Bench.
  - It claims closer spec adherence: additionalProperties defaults to true, and unsupported keywords error out instead of being silently ignored.
  - Grammars are Lark, not GBNF.
  - Source: [docs/llguidance.md](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/docs/llguidance.md)
  - At the pin, common's CMake pulls it via `ExternalProject_Add` from GitHub (v1.0.1, `cargo build --release`), and the glue is `common/llguidance.cpp` (260 lines) — [PIN common/CMakeLists.txt](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/CMakeLists.txt)
- XGrammar:
  - Apache-2.0. Offers Python, C++, JavaScript and Swift APIs. Its `3rdparty/` holds cpptrace, dlpack, googletest (submodules) and picojson — [mlc-ai/xgrammar](https://github.com/mlc-ai/xgrammar), [3rdparty](https://github.com/mlc-ai/xgrammar/tree/main/3rdparty)
  - Its docs describe a "lightweight C++ core" with a portable API — [XGrammar portable API](https://xgrammar.mlc.ai/docs/how_to/portable_api.html)
  - I found no llama.cpp integration.

### Inferences
- **Recommendation:** treat JSG as part of the chat/grammar package the other researcher covers.
  - If Onyx keeps upstream's chat layer, it keeps JSG with it at no extra cost.
  - If Onyx drops libllama-common entirely, the cheapest correct route is to **vendor** `json-schema-to-grammar.cpp` (plus master's `json-schema.cpp` if on master) into `third_party/`, compiled against Onyx's JSON type. That is about 1300–1700 LOC copied, plus a re-pin chore on each bump.
- **A fresh rewrite is not worth it.** Keyword parity, regex→GBNF translation and integer-range rules are the hard parts, and the 1653-line test suite shows the edge-case surface.
- **GBNF cost under Onyx's design.** Onyx runs all slots on one engine thread, so a full-vocab grammar apply (≈150k-token vocab × grammar stacks) on the slow path stalls every concurrent slot for that step. With rejection sampling, the slow path only runs when the sampled token violates the grammar, which for JSON output is a minority of steps. Real-world cost would come from pathological user grammars or very large enum/anyOf schemas.
  - Mitigation inside Onyx: keep the rejection fast path, cap grammar size or complexity on input, and optionally time-limit or abort.
  - llguidance would be the performance fix, but its Rust toolchain dependency directly contradicts the owner's goal. XGrammar adds three submodules and a new grammar dialect. Neither is "at least as good" on the dependency axis.

### Gaps
- I did not find a 2026 benchmark comparing GBNF with llguidance or XGrammar inside llama.cpp.
- I did not verify XGrammar's exact LOC or whether its C++ core builds without Python/nanobind.
- I did not confirm #4218's 2026 status (open or closed).

---

## 3. Tokenize/detokenize/batch/log helpers: are they thin wrappers over llama.h, and how big would replacements be?

### Takeaway
**Verdict: REPLACE all of them.** Each is a 10–25 line two-pass buffer wrapper over a llama.h function. The total Onyx replacement is about 80 LOC, under half a day, with very low risk. Batch helpers must be replaced anyway: **`common_batch_add`/`common_batch_clear` are gone on master**, removed in #29601 in favor of the new `llama_batch_ext` API. Logging needs no replacement once common is gone.

### Cited Findings
- At the pin:
  - `common_tokenize` (common.cpp L1861-L1891) calls `llama_tokenize`, resizes and retries when the result is negative, and throws on INT32_MIN.
  - `common_token_to_piece` (L1893-L1913) does the same two-pass resize with `llama_token_to_piece`, using the SSO buffer first.
  - `common_detokenize` (L1915-L1935) does the same with `llama_detokenize`.
  - Source: [PIN common/common.cpp L1834-L1935](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.cpp#L1834-L1935)
- `common_batch_clear`/`common_batch_add` sit at pin common.cpp L1834-L1859 and fill `llama_batch` fields (token, pos, n_seq_id, seq_id, logits) — [PIN common/common.cpp L1834](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.cpp#L1834)
- On master both functions were removed from common.h by "batch: migrate the rest of examples to llama_batch_ext" ([#29601](https://github.com/ggml-org/llama.cpp/pull/29601)), found via `git log -S common_batch_add`.
  - The new API is `llama_batch_ext_init(ctx)`/`_free`/`_clear`/`_add_token(batch, seq_id, id)`/`_set_pos`/`_set_output_logits`/… — [MASTER include/llama.h L1014-L1090](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/include/llama.h#L1014), added by [#24669](https://github.com/ggml-org/llama.cpp/pull/24669).
  - Classic `llama_batch_init`/`llama_batch_free`/`llama_batch_get_one` are still in master llama.h (L965-L982).
- `common_log_set_verbosity_thold` controls the threshold of common's own `common_log` (common/log.cpp: 459 lines at the pin, 566 on master). Onyx only calls it to silence `LOG_*` output emitted *inside* common (sampling/chat). libllama/ggml logs already go through `llama_log_set`, which Onyx sets in main.cpp — [PIN common/log.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/log.h)
- mtmd has its own log hook: `mtmd_log_set` and `mtmd_helper_log_set` ("this also call mtmd_log_set() internally") — [PIN tools/mtmd/mtmd-helper.h L45-L46](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/mtmd/mtmd-helper.h#L45)

### Inferences
- **Replacement plan, about 80 LOC in a `src/llama_util.h/.cpp`:**
  - `onyx_tokenize`, `onyx_token_to_piece`, `onyx_detokenize`: about 45 LOC.
  - Two batch helpers on the classic `llama_batch`: about 20 LOC.
  - On the next pin bump, migrate to `llama_batch_ext` instead. Either way the common helpers can't be relied on beyond this pin.
  - Replace `common_log_set_verbosity_thold(-1)` with `mtmd_helper_log_set(filter_cb)`, reusing the same WARN+ filter as `llama_log_set`, so mtmd output follows the same policy.
- APIs used: `llama_tokenize`, `llama_token_to_piece`, `llama_detokenize`, `llama_batch_init/free`, then `llama_batch_ext_*`, `llama_log_set`, `mtmd_helper_log_set`.

### Gaps
- I did not check whether `llama_batch` (classic) is marked for deprecation on master.

---

## 4. --fit / auto GPU layers: where it lives, its algorithm, the device memory APIs, and how hard an Onyx version is

### Takeaway
**Verdict: IMPLEMENT AN ONYX VERSION, scoped down.**
- Upstream's fit is `common_fit_params` in **common/fit.cpp**: 1072 LOC at the pin, 1113 on master. It is not in llama.h.
- It works by repeated **trial loads with `no_alloc=true`**, reading a per-buffer-type memory breakdown, then shrinking context and searching layer/tensor placement by false position.
- The breakdown comes from **`llama_get_memory_breakdown`, which is in the private staging header `src/llama-ext.h`**. The other half comes from public ggml-backend calls (`ggml_backend_dev_memory`).
- A single-GPU (plus CPU) Onyx fit covering context reduction, n_gpu_layers search and MoE dense-first is about 250–400 LOC and 2–4 days.
- Risk is medium: Onyx would depend on a WIP staging header, and estimates can drift.
- Multi-GPU tensor_split fitting should stay out of scope.

### Cited Findings
- History:
  - Fit was introduced as the libllama C API `llama_params_fit` in "llama: automatically set parameters not set by the user in such a way that maximizes GPU utilization" ([#16653](https://github.com/ggml-org/llama.cpp/pull/16653)), by JohannesGaessler, merged 2025-12-15.
  - That PR added `no_alloc` dummy model/context loads, "at most six dummy models and contexts" for MoE, and the flags `--fit`, `--fit-ctx` (default 4096) and a per-device margin (default 1024 MiB).
  - It also chose the fitting order: reduce context first, then move weights to system memory, keeping MoE dense weights on GPU first.
  - Fit is on by default, and user-set allocations are left untouched.
  - Source: [PR #16653](https://github.com/ggml-org/llama.cpp/pull/16653)
- Later changelog entries:
  - b7551 ([#18374](https://github.com/ggml-org/llama.cpp/pull/18374)): `llama_params_fit` returns an enum.
  - b7672 ([#18390](https://github.com/ggml-org/llama.cpp/pull/18390)): per-device memory margins.
  - Source: [#9289](https://github.com/ggml-org/llama.cpp/issues/9289)
- The implementation was later moved to libcommon by "fit-params : refactor + add option to output estimated memory per device" ([#22171](https://github.com/ggml-org/llama.cpp/pull/22171), commit "move fit params implementation to libcommon") — [semanticdiff overview of #22171](https://app.semanticdiff.com/gh/ggml-org/llama.cpp/pull/22171/overview). Doxygen for b8951/b9318 shows `common_fit_params` in fit.h — [fossies fit.h](https://fossies.org/dox/llama.cpp-b8951/fit_8h.html)
- Neither the pin's nor master's `include/llama.h` contains `llama_params_fit` or any memory-breakdown function (grep). Both still have `bool no_alloc; // only load metadata and simulate memory allocations` (pin L348, master L355) — [PIN include/llama.h L348](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h#L348)
- Pin `common/fit.h` (67 lines):
  - Declares `common_fit_params(path, mparams, cparams, tensor_split, tensor_buft_overrides, margins, n_ctx_min, extra, log_level)`, which returns SUCCESS/FAILURE/ERROR.
  - Its contract: "only parameters that have the same value as in llama_default_model_params are modified with the exception of the context size which is modified if and only if equal to 0".
  - It is "NOT thread safe because it modifies the global llama logger state".
  - It also declares `common_get_device_memory_data` and `common_memory_breakdown_print`.
  - Source: [PIN common/fit.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/fit.h)
- How one measurement works (`common_get_device_memory_data_impl`, pin fit.cpp L29-L170):
  1. Temporarily swap the llama logger.
  2. Copy mparams with `no_alloc=true` and `load_mode=LLAMA_LOAD_MODE_NONE`.
  3. Load with `llama_model_load_from_file` and create a context with `llama_init_from_model`.
  4. Call `llama_get_memory_breakdown(ctx)`, a `std::map<ggml_backend_buffer_type_t, {model, context, compute}>`.
  5. Attribute each buft to a device via `ggml_backend_buft_is_host` / `ggml_backend_buft_get_device` / `llama_model_get_device(model,i)`.
  6. Read free/total via `ggml_backend_dev_memory`. For the CPU this uses `ggml_backend_dev_by_type(CPU)`; GPUs that report 0/0 are skipped with a warning.
  7. Read the hparams `llama_model_n_layer`, `n_layer_nextn`, `n_ctx_train` and `llama_model_n_expert`, then free everything.
  - Source: [PIN common/fit.cpp L29-L170](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/fit.cpp#L29-L170)
- The fit includes `"../src/llama-ext.h"`, a "staging header for new llama.cpp API. breaking changes and C++ are allowed. everything here should be considered WIP".
  - That header declares `llama_memory_breakdown_data`, `llama_device_memory_data`, `llama_model_n_expert`, `llama_model_n_devices`, `llama_model_get_device` and `llama_get_memory_breakdown`, all as `LLAMA_API`.
  - Upstream's own `tools/mtmd/mtmd-helper-gen.cpp`, `common/speculative.cpp` and `tools/fit-params` also include it.
  - Source: [PIN src/llama-ext.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-ext.h)
- The search (`common_params_fit_impl`, pin fit.cpp L178-L877) runs in four steps:
  1. Measure the default parameters. If n_ctx==0, resolve it to the train context and re-measure.
  2. If memory is short, reduce context by linear interpolation between n_ctx_min and max. It re-measures, rounds for CUDA/stream alignment, and is more conservative for multiple devices.
  3. Abort if the user already set n_gpu_layers, tensor_split or tensor_buft_overrides. Otherwise fill "dense" layers back to front, with all MoE expert tensors in system memory. The per-device layer count is found by "the method of false position" between a low and a high configuration, stopping at a 1-layer difference.
  4. For MoE, convert dense-only layers to full layers front to back until devices are full, then try to fit part of one more layer (attn / up / gate fractions via `LAYER_FRACTION_*` regexes).
  - SPLIT_MODE_TENSOR and ROW are not supported.
  - Source: [PIN common/fit.cpp L178-L877](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/fit.cpp#L178)
- Defaults in common: `fit_params=true`, `fit_params_min_ctx=4096`, `fit_params_target` = 1 GiB per device, `n_gpu_layers=-1` ("auto") — [PIN common/common.h L470-L481](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.h#L470-L481). Fit runs inside `common_init_result` before the real `llama_model_load_from_file` — [PIN common/common.cpp L1290-L1330](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.cpp#L1290)
- Public ggml-backend device APIs (pin `ggml/include/ggml-backend.h`, 441 lines; master 463):
  - `ggml_backend_dev_count/get/by_type`
  - `ggml_backend_dev_memory(dev,&free,&total)` (master L185)
  - `ggml_backend_dev_get_props` (`struct ggml_backend_dev_props`, master L164/L187)
  - `ggml_backend_dev_type`/`name`/`description`/`buffer_type`
  - `ggml_backend_buft_name`/`is_host`/`get_device`
  - `ggml_backend_load_all`
  - Source: [MASTER ggml-backend.h](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/ggml/include/ggml-backend.h#L164)
- `llama_max_devices()` and `llama_max_tensor_buft_overrides()` are public (pin llama.h L555-L557). `llama_model_n_layer_nextn` is public (L593).
- Master fit.cpp changes since the pin: +41 lines, including "llama : report compute graph reallocations during inference" ([#30329](https://github.com/ggml-org/llama.cpp/pull/30329)). An auto-fitting context change for unified KV ([#28849](https://github.com/ggml-org/llama.cpp/pull/28849)) was reverted ([#29437](https://github.com/ggml-org/llama.cpp/pull/29437)), per `git log` pin..master.
- Onyx today sets `mparams.n_gpu_layers = 999` when `-ngl` is unset and uses n_ctx default 4096. It has no fit, so an oversized model fails at load (src/engine.cpp L69-L76, src/args.h L32-L34).

### Inferences
- **Onyx fit design**, `src/fit.cpp` at about 250–400 LOC:
  1. Do a `no_alloc` dry load (model + context with the chosen cparams) and call `llama_get_memory_breakdown` via `#include "../src/llama-ext.h"`, exactly as upstream common/mtmd do. The symbols are exported `LLAMA_API` from libllama, so no common/ is needed.
  2. Read free memory per device with `ggml_backend_dev_memory`.
  3. If it doesn't fit: (a) reduce n_ctx toward a floor such as 4096, by interpolation as upstream does. (b) Binary-search `n_gpu_layers` over [0, n_layer+1] with dry loads. Each dry load costs a GGUF metadata parse plus graph reserve; upstream accepts up to about 6 for MoE. (c) For MoE (`llama_model_n_expert>0`), first try all experts on CPU (`LLM_FFN_EXPS_REGEX` override, see §5) with all layers on GPU, then search N for "experts of the first N layers on CPU".
  4. Apply the result to the real load.
- Skip multi-GPU tensor_split optimisation and partial-layer fractions. Those account for most of upstream's 1072 lines.
- **Risks:**
  - llama-ext.h is explicitly WIP. The memory-breakdown part was unchanged pin→master (master only added declarations), but it is C++ (`std::map`) and may be reshaped.
  - Fallback if it disappears: estimate from GGUF tensor sizes (`gguf_*` in ggml) plus a computed KV size. That is less accurate, since compute-buffer size is unknown, and is the reason upstream uses dry loads.
  - The fit swaps the global llama logger, so run it before the engine thread starts.
- Ship it gated behind `--fit on|off`, defaulting to on only when `-ngl`, `--ctx-size` and overrides are unset, to match upstream semantics.

### Gaps
- I did not read the rest of the #16653 thread (beyond 100k characters) for later algorithm revisions.
- I did not confirm the exact merge build of #22171.
- I did not measure dry-load cost on real models.

---

## 5. MoE offload: how upstream builds tensor_buft_overrides for -ot / --cpu-moe / --n-cpu-moe, and how big an Onyx version is

### Takeaway
**Verdict: REPLACE with Onyx code, about 50–80 LOC, half a day, low risk.** Upstream only builds `{regex_pattern, buffer_type}` pairs. **libllama itself does the `std::regex_search` matching** during loading. The CPU buft comes from `ggml_backend_cpu_buffer_type()`, and named bufts come from enumerating ggml-backend devices. Note: master adds an experimental `moe_cache_size` (a GPU cache for host-resident experts), which Onyx could expose later with no common/ code.

### Cited Findings
- Regex constants:
  - `LLM_FFN_EXPS_REGEX = "\\.ffn_(up|down|gate|gate_up)_(ch|)exps"`
  - `LLM_FFN_DENSE_REGEX = "\\.ffn_(up|down|gate)\\."`
  - `llm_ffn_block_regex(idx, re)` = `"blk\\.%d" + re`
  - `llm_ffn_exps_cpu_override()` = `{LLM_FFN_EXPS_REGEX, ggml_backend_cpu_buffer_type()}`
  - `llm_add_n_cpu_ffn_overrides(n, re, v)` pushes one per-layer pattern for layers 0..n-1, keeping the strings alive in a static `std::list`.
  - Source: [PIN common/common.h L1127-L1149](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.h#L1127)
- CLI flags (pin arg.cpp L2776-L2808): `-ot/--override-tensor <pattern>=<buft>,…`, `-cmoe/--cpu-moe`, `-ncmoe/--n-cpu-moe N`, `-ncffn/--n-cpu-ffn N` — [PIN common/arg.cpp L2776](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/arg.cpp#L2776)
- `parse_tensor_buffer_overrides` (pin arg.cpp L252-L284) works like this: call `ggml_backend_load_all()`, then for each `ggml_backend_dev_get(i)` take `ggml_backend_dev_buffer_type(dev)` and map it by `ggml_backend_buft_name`. Split on `,`/`=`, print the available bufts on an unknown name, and store the pattern strings in a static list — [PIN common/arg.cpp L252-L284](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/arg.cpp#L252)
- The array passed to `mparams.tensor_buft_overrides` must end with a `{nullptr,…}` sentinel; common asserts "Tensor buffer overrides not terminated with empty pattern" — [PIN common/common.cpp ~L1704-L1708](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/common.cpp#L1704)
- libllama does the matching: `std::regex pattern(overrides->pattern); if (std::regex_search(tensor_name, pattern))` — [PIN src/llama-model-loader.cpp ~L1231](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-model-loader.cpp#L1231)
- `struct llama_model_tensor_buft_override` is in public llama.h (pin L308, field at L318) and has been there since b5028 ([#11397](https://github.com/ggml-org/llama.cpp/pull/11397)) per [#9289](https://github.com/ggml-org/llama.cpp/issues/9289).
- Master adds `size_t moe_cache_size; // device cache in bytes for the experts kept in host memory … [EXPERIMENTAL]` to model params. This came from "llama : add a GPU cache for MoE experts kept in host memory" ([#29887](https://github.com/ggml-org/llama.cpp/pull/29887)) and "llama: support MoE cache over multiple GPUs" ([#30112](https://github.com/ggml-org/llama.cpp/pull/30112)) — [MASTER include/llama.h](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/include/llama.h). Also related: "ggml: refactor selective expert copying to user code" ([#29943](https://github.com/ggml-org/llama.cpp/pull/29943)) touched ggml-backend.h.

### Inferences
- **Onyx version, about 50–80 LOC in args.cpp/engine.cpp:**
  - Flags `--cpu-moe`, `--n-cpu-moe N`, `-ot pat=buft,…`.
  - A `std::vector<std::string>` of owned patterns plus a `std::vector<llama_model_tensor_buft_override>` with the null terminator, both held by `onyx_engine` for the model's lifetime. This avoids upstream's static-list leak pattern.
  - Buft lookup via `ggml_backend_dev_count/get/buffer_type/buft_name`, with the CPU buft from `ggml_backend_cpu_buffer_type()` (`ggml-cpu.h`).
  - The same helper feeds the fit search in §4.
- APIs used: `llama_model_params.tensor_buft_overrides`, `llama_max_tensor_buft_overrides`, `ggml_backend_dev_*`, `ggml_backend_buft_name`, `ggml_backend_cpu_buffer_type`, `ggml_backend_load_all` (only for dynamic-backend builds).
- Tensor naming (`blk.N.ffn_*_exps`) is an informal convention between GGUF converters and libllama. It has been stable, but it is not an API, so it is a low-to-medium long-term risk.

### Gaps
- I did not evaluate `moe_cache_size` performance or whether it is stable enough to expose.

---

## 6. After replacing these and the chat layer, could Onyx stop linking common/ entirely? What else ties the CMake to common, and what does mtmd-helper depend on?

### Takeaway
**Yes, technically: libllama and mtmd build with `LLAMA_BUILD_COMMON=OFF`.** But one large non-chat tie was not in the brief: **`common_json` (common/json.h + json.cpp, 433 LOC) is used 160 times in server.cpp.** It is a pimpl wrapper over vendored nlohmann/json.
- To drop common, Onyx must switch to nlohmann directly or another JSON library. `vendor::nlohmann` is still added by llama.cpp's vendor/CMakeLists even when common is off, though Onyx owning a copy in third_party/ is cleaner.
- mtmd/mtmd-helper do **not** depend on common. Upstream enforces this with a CMake check.
- The `build-info` tie is already gone in Onyx.

### Cited Findings
- Root CMake:
  - `option(LLAMA_BUILD_COMMON … ${LLAMA_STANDALONE})` (L129)
  - `add_subdirectory(vendor)` is unconditional (L227); `add_subdirectory(common)` only runs if LLAMA_BUILD_COMMON (L229)
  - `LLAMA_BUILD_MTMD` builds tools/mtmd standalone when `NOT (LLAMA_BUILD_COMMON AND LLAMA_BUILD_TOOLS)` (L256-L258)
  - Source: [PIN CMakeLists.txt](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/CMakeLists.txt#L129)
- `vendor/CMakeLists.txt`: "mtmd needs these even when common is not built" → hash, miniaudio, nlohmann, sheredom, stb. Only cpp-httplib is "only used by common". `vendor::nlohmann` is a header-only INTERFACE target exposing `<nlohmann/json.hpp>` (25,526 lines) — [PIN vendor/CMakeLists.txt](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/vendor/CMakeLists.txt)
- mtmd:
  - It links `PUBLIC ggml llama` and `PRIVATE Threads vendor::hash vendor::miniaudio vendor::stb vendor::sheredom`.
  - The CMake carries a guard: "if mtmd is linked against llama-common, we throw an error … It must not link against llama-common" (tools/mtmd/CMakeLists.txt L82-L83, L130-L135).
  - `mtmd-helper.cpp` includes only mtmd.h, mtmd-helper.h, mtmd-helper-common.h, llama.h, hash/hash.h, miniaudio, stb_image and sheredom/subprocess.
  - Source: [PIN tools/mtmd/CMakeLists.txt](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/tools/mtmd/CMakeLists.txt)
- `llama-common` links `PUBLIC vendor::nlohmann vendor::sheredom llama-common-base llama Threads` and `PRIVATE cpp-httplib` (plus optional llguidance). `llama-common-base` is the generated `build-info.cpp` — [PIN common/CMakeLists.txt](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/CMakeLists.txt)
- `common/json.h` describes itself as "common_json, a thin wrapper around vendor json library … pimpl, we are using nlohmann::json for now". Object keys keep insertion order, and errors are thrown as `common_json_error` — [PIN common/json.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/json.h)
- Onyx's CMakeLists:
  - Forces `LLAMA_BUILD_COMMON ON`.
  - Adds `${ONYX_LLAMA_SRC}/common` to the include path.
  - Links `onyx-httplib llama-common llama mtmd`. It deliberately orders onyx-httplib first because llama-common re-exports about 1200 httplib symbols.
  - Already has its own pin string instead of upstream `build-info` (`/home/user/Onyx-Engine/CMakeLists.txt`).
- `common/fit.cpp` and `common/speculative.cpp` reach into `../src/llama-ext.h`. Upstream's own non-common code (mtmd-helper-gen.cpp, tools/fit-params) does the same, so this staging header is used outside common too — [PIN src/llama-ext.h](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/src/llama-ext.h)

### Inferences
- **To drop common:**
  1. Replace the items in §1–§5.
  2. The chat layer is covered by the other researcher. Note it pulls in json-schema-to-grammar, the jinja engine and peg-parser.
  3. Replace `common_json` with nlohmann directly (`nlohmann::ordered_json` keeps the insertion-order behavior), or use Onyx's own vendored JSON in third_party/. This is a mechanical rename across about 160 sites plus error-type changes (`common_json_error` → `nlohmann::json::exception`). Effort is about 0.5–1 day.
  4. Set `LLAMA_BUILD_COMMON OFF`, drop the common include dir and the `llama-common` link.
- **Bonus:** the httplib symbol-ordering hack and the ODR concern in Onyx's CMake go away, because cpp-httplib is only built for common.
- `build-info` is already decoupled. Nothing in mtmd needs common.

### Gaps
- I did not test a build with `LLAMA_BUILD_COMMON=OFF` (not run, per constraints).
- I did not check whether `llama_add_compile_flags()` or other root CMake helpers behave differently with common off.

---

## 7. How stable are llama.h and ggml-backend.h across pin bumps compared to common/?

### Takeaway
Over the 1072 commits from pin to master, **llama.h changed mostly by addition (+120/−6 lines)**: one return-type change, version-macro bumps, and comment edits. ggml-backend.h was reorganized but kept every function Onyx would use. **common/ churned much more**, about 1200 changed lines across the files Onyx touches, including **removals Onyx would hit**: `common_batch_add/clear` removed (#29601), and the `json_schema_to_grammar` header/API reshaped (#28736, #30210). Owning thin code on llama.h/ggml-backend.h therefore *reduces* pin-bump breakage. The exception is anything resting on `src/llama-ext.h`, which is explicitly unstable.

### Cited Findings
- `git diff --stat pin..master`:

  | File | Lines changed |
  |---|---|
  | include/llama.h | 120 (+, −6) |
  | ggml/include/ggml-backend.h | 44 |
  | ggml/include/ggml.h | 85 |
  | src/llama-ext.h | +32 |
  | common/common.cpp | 702 |
  | common/common.h | 184 |
  | common/json-schema-to-grammar.cpp | 604 |
  | common/sampling.cpp | 180 |
  | common/log.cpp | 149 |
  | common/fit.cpp | 41 |
  | tools/mtmd/mtmd.h | 92 |
  | tools/mtmd/mtmd-helper.h | 77 |

  Source: local git on [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp)
- Lines removed from llama.h pin→master:
  - `LLAMA_SESSION_VERSION` 10→11 and `LLAMA_STATE_SEQ_VERSION` 3→4
  - The `embeddings` comment reworded, the numa comment reworded
  - `llama_sampler_chain_n` return type `int`→`int32_t` ([#28631](https://github.com/ggml-org/llama.cpp/pull/28631))
  - Everything else is additions: `llama_batch_ext` ([#24669](https://github.com/ggml-org/llama.cpp/pull/24669)), `moe_cache_size` ([#29887](https://github.com/ggml-org/llama.cpp/pull/29887)), and 16 commits in total
  - Source: [MASTER include/llama.h](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/include/llama.h)
- ggml-backend.h: the removed `ggml_backend_buft_*` lines were re-added (realigned) when the buft interface gained `alloc_buffer_n` ([#23671](https://github.com/ggml-org/llama.cpp/pull/23671)). `ggml_backend_buft_name`/`is_host`, `ggml_backend_dev_memory`, `dev_get_props`, `dev_count/get/by_type` and `load_all` are all present on master. Five commits touched the file — [MASTER ggml-backend.h](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/ggml/include/ggml-backend.h)
- The libllama API changelog issue #9289 exists so that "third-party projects that depend on libllama" can track changes before upgrading.
  - Recent entries are mostly additive (e.g. `llama_get_causal_attn` b11223, `load_mtp` b10206).
  - It records sampler-signature breaks: `n_ctx_train` removed from `llama_sampler_init_dry` (b10273, [#26524](https://github.com/ggml-org/llama.cpp/pull/26524)), and `n_vocab` moved to `llama_sampler_init_penalties` (b10258, [#26520](https://github.com/ggml-org/llama.cpp/pull/26520)).
  - An Oct 10, 2026 comment proposes a "v0.6.0" entry for `llama_batch_ext`; it is not yet merged into the table.
  - Source: [#9289](https://github.com/ggml-org/llama.cpp/issues/9289)
- mtmd-helper.h on master removed `mtmd_helper_support_video`, `mtmd_helper_post_decode_callback` and `mtmd_helper_model_can_chat`. The four functions Onyx uses (`mtmd_helper_eval_chunk_single`, `bitmap_init_from_buf`, `get_n_pos`, `init_opt_default`) are still present — [MASTER tools/mtmd/mtmd-helper.h](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/tools/mtmd/mtmd-helper.h)

### Inferences
- **Stability ranking for Onyx dependencies, most to least stable:**
  1. ggml-backend.h device APIs
  2. llama.h (additive; occasional sampler-init signature tweaks, documented in #9289)
  3. mtmd-helper.h (some removals, but not of Onyx's functions)
  4. src/llama-ext.h (WIP by declaration)
  5. common/ (no changelog, frequent renames and removals)
- An Onyx sampler wrapper would be exposed to the occasional `llama_sampler_init_*` signature change. Those are listed in #9289 and are compile-time errors, not silent breakage.
- **Overall verdicts:**

  | Item | Verdict | Size | Effort |
  |---|---|---|---|
  | Sampler | Replace | ~150–200 LOC | ~1 day |
  | Tokenize/detok/piece/batch/log helpers | Replace | ~80 LOC | <0.5 day; batch helpers are mandatory on bump |
  | MoE offload flags | Implement | ~50–80 LOC | 0.5 day |
  | Fit | Implement, scoped | ~250–400 LOC | 2–4 days; depends on llama-ext.h |
  | JSON-schema→grammar | Keep upstream code; vendor a copy if common is dropped | ~1300–1700 LOC copied | — |
  | `common_json` → nlohmann | Required to fully unlink common | ~160 sites | 0.5–1 day |

### Gaps
- No #9289 entry covers `llama-ext.h` changes, so I could not quantify how often staging APIs break historically beyond pin→master (+32 additive lines).
- I did not compute ABI stability, since Onyx links statically.
