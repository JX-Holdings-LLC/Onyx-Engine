# Chat templating, tool-call parsing and reasoning parsing: an Onyx-owned chat layer

Scope: how Onyx Engine could replace llama.cpp `common/` for chat templating (Jinja), chat-format detection, tool-call and reasoning parsing, streaming deltas, lazy tool-call grammars and JSON, with a replace-or-keep verdict for each part. Research date 2026-10-21.

Method: I sparse-cloned llama.cpp at the Onyx pin `9723942adc518b43c4b95dc4dce6906903eb5e09` (2026-08-30) and at master `339f3242c761f16c27e5ffcde1d35dcc31933a55` (2026-10-20). I also cloned Ollama `2cd63f7a` (2026-10-20), vLLM `7e3d5595` (2026-10-20), mistral.rs `4c56e256` (2026-10-20) and google/minja `021c2293` (2025-09-22). The code was read but not run. Line counts come from `wc -l` on those checkouts. Links below point at the same commits.

Pin links use this base: `https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/` (written below as PIN). Master links use this base: `https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/` (written below as MASTER).

## Q1. What upstream common/ contains for this, at the pin and on master, and how big each part is

### Takeaway
Upstream no longer uses minja. Since January 2026 it has shipped its own Jinja engine in `common/jinja/`, about 6.3k lines. Around it sits a three-layer chat stack:
- string-sniffing detection that routes about 20 families to hand-written handlers;
- a generic "autoparser" for every other template, which renders the template differentially to infer the format;
- a PEG parser that does the parsing and also emits the GBNF for lazy tool-call grammars.

All the chat-specific code (Jinja, chat, autoparser, PEG and per-family parsers) totals about 17.8k lines at the pin and about 19.1k on master. The churn is in the chat/parser layers, not in Jinja.

### Cited findings
- **The Jinja engine is upstream's own.** PR #18462 by ngxson, "common : implement new jinja template engine", opened 2025-12-29 and merged 2026-01-16 after 137 commits. Its history includes "rm minja". On the author's 370-template suite it fails 14 templates, against 8 for minja, a gap the author called acceptable — [PR #18462](https://github.com/ggml-org/llama.cpp/pull/18462).
- **Engine design.** The engine is "originally inspired by huggingface.js's jinja package". Its parts are a lexer, a parser that builds an AST, a runtime that walks the AST, and a value layer. It is "decoupled from the JSON library: common_json is only used for JSON-to-internal type translation". It has "input marking" to stop special tokens injected through user content: each string carries an `is_input` flag — [PIN common/jinja/README.md](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/jinja/README.md).
- **Jinja size at the pin: 6,349 lines across 13 files.** The biggest are value.cpp (1,586), runtime.cpp (1,003), runtime.h (788), value.h (759), parser.cpp (602), caps.cpp (541) and lexer.cpp (341). Only `value.cpp/.h` and `caps.cpp` reference `common_json` — [PIN common/jinja/](https://github.com/ggml-org/llama.cpp/tree/9723942adc518b43c4b95dc4dce6906903eb5e09/common/jinja).
- **Jinja size on master: 6,596 lines**, so +247 in seven weeks. value.cpp is now 1,758 and caps.cpp 592 — [MASTER common/jinja/](https://github.com/ggml-org/llama.cpp/tree/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/jinja).
- **Chat layer at the pin:**
  - `chat*.cpp/h` total 8,788 lines. The pieces are chat.cpp (3,915), chat-diff-analyzer.cpp (1,635), chat-peg-parser.cpp (1,232), chat-auto-parser-generator.cpp (508), chat-auto-parser.h (453), chat.h (388), chat-auto-parser-helpers.cpp (363) and chat-peg-parser.h (220).
  - `peg-parser.cpp/h` adds 2,654 lines.
  - At the pin, per-family handlers live inside chat.cpp as `static common_chat_params_init_*` functions: ministral_3, qwen3_coder, gpt_oss, gemma4, functionary_v3_2, kimi_k2, lfm2, gigachat_v3, deepseek_v3_2, kimi_k3, cohere2moe, minimax_m3, minicpm5, muse_glimmer and more.
  - Source: [PIN common/chat.cpp](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/chat.cpp).
- **Chat layer on master:**
  - chat.cpp shrank to 1,782 lines and `chat*` totals 6,733. The handlers moved to a new `common/parsers/` directory: 23 files and 3,360 lines.
  - That directory holds cohere2moe, deepseek, functionary-v3-2, gemma4, gigachat-v3, gpt-oss, harmony-xml, k2-horizon, kimi-k2, kimi-k3, lfm2, ling3, llm-jp-harmony, minicpm5, minimax-m3, ministral3, muse-glimmer, muse-spark, qwen3-coder and translate-gemma.
  - peg-parser is 2,379 lines.
  - Source: [MASTER common/parsers/](https://github.com/ggml-org/llama.cpp/tree/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/parsers).
- **Format detection is substring matching on the template source.**
  - `common_chat_template_detect_params_init(src)` tests, in order: Ministral 3 (`[SYSTEM_PROMPT]`+`[TOOL_CALLS]`+`[ARGS]`, no `[CALL_ID]`), llm-jp-harmony, harmony-xml, gpt-oss (`<|channel|>`), muse-spark, muse-glimmer, functionary v3.2 (`>>>all`), Kimi K2 (`<|tool_calls_section_begin|>`), Kimi K3 (`<|open|>`/`<|close|>`), K2-Horizon, Ling3 (`<arg_key>`), Cohere2MoE, LFM2/LFM2.5, GigaChat3, MiniMax M3 (`<invoke name=`), DeepSeek V3.2/V4 (`DSML`), Gemma 4 (`'<|tool_call>call:'`, with a legacy-template workaround), MiniCPM5, TranslateGemma and Qwen3-Coder.
  - The Qwen3-Coder test checks `<tool_call>`+`<function=`+`<parameter=`. The code comment says this format is "also used by Nemotron Nano 3, Qwen3.5 and StepFun-3.5-Flash".
  - When nothing matches, `common_chat_template`'s constructor runs `autoparser::analyze_template` once.
  - Source: [MASTER common/chat.cpp L1092-L1240](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/chat.cpp).
- **The autoparser classifies templates along four axes.** Each axis has an enum:
  - `reasoning_mode`: NONE, TAG_BASED `<think>…</think>`, …
  - `content_mode`: PLAIN, ALWAYS_WRAPPED, WRAPPED_WITH_REASONING
  - `call_id_position`: PRE_FUNC_NAME, BETWEEN_FUNC_AND_ARGS, POST_ARGS
  - `tool_format`: JSON_NATIVE `{"name":…,"arguments":…}`, TAG_WITH_JSON `<function=X>{…}`, TAG_WITH_TAGGED `<param=key>value`
  - It works by rendering template variants and diffing them (`diff_split{prefix,suffix,left,right}`, `compare_variants_result`) — [MASTER common/chat-auto-parser.h](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/chat-auto-parser.h).
- **The parser also produces the grammar.** In the per-family handlers the same PEG arena drives both: `data.grammar = build_grammar([&](…){ data.parser.build_grammar(builder, data.grammar_lazy); })` in qwen3-coder.cpp L180 — [MASTER common/parsers/qwen3-coder.cpp](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/parsers/qwen3-coder.cpp). json-schema-to-grammar.cpp is a separate 1,264 lines at the pin and 1,028 on master.
- **Final format enum on master:** CONTENT_ONLY, PEG_SIMPLE, PEG_NATIVE, PEG_GEMMA4 and PEG_MINIMAX_M3. All parsing now goes through the PEG engine — [MASTER common/chat.h L236-L244](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/chat.h).
- **API break after the pin.**
  - Pin: `common_chat_parse(const std::string & input, bool is_partial, const common_chat_parser_params &)`.
  - Master: `common_chat_parse(const common_chat_input & input, …)`. `common_chat_template_direct_apply` now returns `common_chat_prompt`.
  - Master adds a `common_chat_session` class with `feed(chunk)`, `finish()`, `apply_sampling(common_params_sampling&)`, `grammar()`, `thinking_start_tag()` and `thinking_end_tags()`.
  - Sources: [PIN common/chat.h L340](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/chat.h); [MASTER common/chat.h L388, L396-L424](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/chat.h).
- **PR #29876, "server : accumulate generated text and tokens as parse input"**, merged 2026-10-07. It adds `common_chat_input`, which holds text plus a parallel per-byte token array so parsers can match on token IDs. If the re-assembled pieces don't match the text, the tokens are dropped — [PR #29876](https://github.com/ggml-org/llama.cpp/pull/29876).
- **PR #30210, "chat : refactor API"** by aldehir, merged 2026-10-09. It introduces `common_chat_session`, moves template detection, autoparser analysis and tokenization to run once per template, removes PEG JSON serialization, and drops fields from `common_chat_parser_params`. A reviewer reported that `continue_final_message`/prefill no longer echoes the prefilled text unless `echo` is set. The author describes the core flow as unchanged and open to a later, separate refactor — [PR #30210](https://github.com/ggml-org/llama.cpp/pull/30210).
- **Upstream test investment:**
  - tests/test-chat.cpp is 8,188 lines, test-chat-auto-parser.cpp 2,706, test-chat-peg-parser.cpp 1,106, test-chat-template.cpp 738 and test-chat-analysis.cpp 615, for 13,353 in total.
  - test-chat.cpp references 65 distinct files in `models/templates/`. That directory holds 75 reference templates on master and 69 at the pin.
  - Source: [MASTER tests/](https://github.com/ggml-org/llama.cpp/tree/339f3242c761f16c27e5ffcde1d35dcc31933a55/tests).
- **How Onyx uses this today** ([src/server.cpp](../../src/server.cpp)):
  - `common_chat_msgs_parse_oaicompat`, `common_chat_tools_parse_oaicompat` and `common_chat_tool_choice_parse_oaicompat` in `parse_chat_inputs`, L478-L541.
  - `common_chat_templates_apply` at L748.
  - `common_chat_parser_params` and `common_chat_msg prev_msg` in `chat_stream_state`, L544-L552.
  - `common_chat_msg_diff` mapped to OpenAI deltas in `diff_to_delta`, L181-L211.
  - `cp.grammar`, `cp.grammar_lazy`, `cp.grammar_triggers` and `cp.preserved_tokens` in `apply_grammar`, L398-L429. This includes converting WORD triggers that are a single preserved token into TOKEN triggers.
  - `cp.thinking_start_tag`/`thinking_end_tags` and a check of whether the prompt ends with `<think>`, in `build_reasoning_budget`, L437-L475.
  - `common_chat_templates_init` in [src/engine.cpp L172](../../src/engine.cpp).

### Inferences
- The size Onyx would actually take on is roughly 6.3k lines of Jinja plus about 11k lines of chat, autoparser and PEG code at the pin. That 11k excludes json-schema-to-grammar. Upstream backs it with about 13k lines of tests. In seven weeks the layout was reorganised: chat.cpp went from 3,915 to 1,782 lines, parsers/ was added and the session API arrived. Pin bumps break the chat layer, not the Jinja engine. The Jinja engine is comparatively stable, with +4% lines and the same file set.
- The autoparser is upstream's real moat. It is why llama.cpp handles a never-before-seen Hermes-style or GLM-style template without new code. A hand-written registry gives this up and has to fall back to "content only" for unknown templates.

### Gaps
- I did not run the line-history (git log) for each file because the clones are shallow, so per-file churn is inferred from pin-to-master diffs only.
- I did not read the remaining 35k characters of PR #18462 or every follow-up regression report.

## Q2. Jinja options: write an Onyx subset interpreter, vendor a standalone library, or keep upstream's?

### Takeaway
Real 2026 templates use a fairly wide Jinja subset, measured below across 75 templates. Upstream needed 6.3k lines and 137 commits to get to 356 of 370 templates. minja is effectively orphaned. The cheapest robust path is to vendor a frozen snapshot of upstream's `common/jinja/` into an Onyx-owned directory, pulling fixes deliberately. That copy is MIT, about 6.3k lines, and only lightly tied to common_json. Writing a new interpreter would take weeks and would be worse. minja is the only credible third-party drop-in, and it is weaker on maintenance and security.

### Cited findings
- **Feature census** over the 75 templates in [MASTER models/templates/](https://github.com/ggml-org/llama.cpp/tree/339f3242c761f16c27e5ffcde1d35dcc31933a55/models/templates). These are rough grep counts; the filter counts also pick up `|im_start`-style token text, which is excluded below.
  - **Statements:** `if`/`elif`/`else` (1,734 ifs), `set` (1,525, plus 13 block `set…endset`), `for` (464), `macro` (154), `break` (6), `filter` (1) and `generation` (5).
  - **Filters:** tojson (168), length (128), trim (78), items (70), default (56), string (52), list (40), replace (38), join (28), lower (18), upper (17), rejectattr (16), safe (15) and dictsort (12).
  - **Methods:** `.get(` (162), `.split(` (68), `.startswith(` (42), `.strip(` (38), `.items(` (36), `.lstrip(` (29), `.format(` (21), `.endswith(` (15), `.rstrip(` (14), `.replace(` (7), `.pop(` (3) and `.append(` (2).
  - **Tests:** `is defined` (328), `is string` (182), `is mapping` (141), `is not none` (88), `is none` (40), `is iterable` (40), `is sequence` (25), `is boolean`, `is true`/`false` and `is sameas`.
  - **Other:** `namespace(` (146), `loop.last`/`index`/`first`/`previtem`/`nextitem`, slices such as `[1:]`, `strftime_now` (12), and `raise_exception` in 25 of the 75 files.
- **minja status:**
  - google/minja's last commit is 2025-09-22, with 3,627 lines (minja.hpp 3,077 plus chat-template.hpp 550). It is MIT-style licensed ("Copyright 2024 Google LLC", permission notice) and header-only C++17. It depends on nlohmann::json (`ordered_json`).
  - Its README warns that "@ochafik has left Google, watch out for https://github.com/ochafik/minja", and says use is "at your own risk". Non-goals include addressing prompt-injection risks and full Jinja compliance.
  - Sources: [google/minja README](https://github.com/google/minja/blob/021c2293c187789ef13d56c6cfd89c9b134fd80f/README.md), [LICENSE](https://github.com/google/minja/blob/021c2293c187789ef13d56c6cfd89c9b134fd80f/LICENSE).
- **The ochafik/minja fork** says "I'll only maintain my fork from now on" and shows 305 commits. The fetched page did not show the last-commit date or license, so I could not confirm recent activity — [ochafik/minja](https://github.com/ochafik/minja).
- **minja's own README suggests other engines** for general needs: Jinja2Cpp, Jinja2CppLight and inja ("none of which we endorse"). It describes minja as "not general purpose" and limited to what chat templates need — [google/minja README](https://github.com/google/minja/blob/021c2293c187789ef13d56c6cfd89c9b134fd80f/README.md).
- **mistral.rs uses the Rust minijinja crate**, `minijinja = "2.14.0"` with builtins/json/preserve_order and `minijinja-contrib` with `pycompat`. That is a mature general Jinja engine, not a home-grown subset, and is not usable from C++ — [mistral.rs Cargo.toml L119-L120](https://github.com/EricLBuehler/mistral.rs/blob/4c56e256dea8fa61f7457aedacc6ce5a96a4ffdd/Cargo.toml).
- **Ollama avoids Jinja entirely.** It uses Go `text/template` templates by default. For newer models it uses hand-written Go "renderers" that bypass templates, for example qwen3.5, deepseek3.1, laguna and gemma4 — [DeepWiki: Ollama template system](https://deepwiki.com/ollama/ollama/7.1-template-system); [Ollama template docs](https://docs.ollama.com/template). A Hugging Face forum thread notes that converting HF Jinja to Ollama's Go format loses information — [HF forum](https://discuss.huggingface.co/t/ollama-model-registry-provides-wrong-chat-template/176139).
- **Upstream compatibility numbers.** Upstream's engine fails 14 of 370 templates and minja fails 8 of 370 ([PR #18462](https://github.com/ggml-org/llama.cpp/pull/18462)). Fidelity is a long tail even for a project that sees every new model on day one.

### Inferences
- **Option A: vendor-fork upstream `common/jinja` (recommended).**
  - Copy the 13 files (about 6.3k lines, MIT) into e.g. `src/chat/jinja/`. Swap `common_json` for nlohmann (see Q6) in the three files that use it, and build them as part of Onyx.
  - Effort is about 2-4 days, including porting `tests/test-jinja`-style golden tests.
  - Quality is the same as today, keeps input marking and is decoupled from pin bumps. Fixes are cherry-picked when a new model's template fails.
  - The risk is drift: Onyx has to watch upstream's jinja/ commits. The diff is small, about 250 lines in seven weeks.
- **Option B: vendor minja (ochafik fork).**
  - About 3.6k lines, header-only, 1-2 days to integrate.
  - It has slightly better measured compatibility (8 vs 14 failures in one suite, Jan 2026) but no input marking.
  - The maintainer is a single person, and it no longer has llama.cpp's daily exposure to new templates.
  - New templates have since added features, such as Gemma 4 interleaved and DeepSeek V4, that minja may not have tracked. That is unverified.
- **Option C: write an Onyx subset interpreter.**
  - The census shows the "subset" still needs: a full expression grammar with slicing, `namespace`, macros with arguments, about 15 filters, about 12 string/dict methods, about 10 tests, loop variables, whitespace control (`{%-`/`-%}`), `tojson` with Python-compatible spacing and ordering, `raise_exception` and `strftime_now`.
  - Realistic size is 3-5k lines and 3-6 weeks to reach parity on 70+ reference templates.
  - It would almost certainly start worse than upstream, which needed 137 commits. Not recommended.
- **Option D: Ollama-style hand-written renderers, no Jinja.** Covered in Q4. It only makes sense for a small, curated model list. It loses GGUF-embedded template support, `--chat-template` overrides and `chat_template_kwargs`.
- **Jinja2Cpp** is general-purpose, heavier and needs extra dependencies; inja is a different template dialect. I did not verify either one's licence, size or 2026 status in this pass, so treat them as unassessed.

### Gaps
- I could not confirm the ochafik/minja last-commit date or licence, or Jinja2Cpp's current maintenance and size. The gh API was blocked in this session and the web fetch didn't show them.
- No independent compatibility run of minja against 2026 templates (Qwen3.5, Gemma 4, DeepSeek V4, Kimi K3) was done.

## Q3. Tool-call and reasoning output formats per family; how many distinct parsers are needed

### Takeaway
Popular 2026 open models use about 10-12 distinct tool-call wire formats and about 4 reasoning conventions. Every independent project ends up with a similar list of per-family parsers:
- upstream llama.cpp: about 20 specialised handlers plus the autoparser;
- Ollama: 20 parsers;
- mistral.rs: 9 formats;
- vLLM: 53 tool parsers and 38 reasoning parsers.

A practical Onyx set for the families the brief names is about 9 tool parsers, about 3 reasoning modes and 1 generic tagged-JSON fallback.

### Cited findings
- **Upstream specialised handlers** (substring detection, Q1) cover:
  - Ministral 3 / Devstral 2 (`[TOOL_CALLS]name[ARGS]{…}`);
  - gpt-oss harmony (`<|channel|>`) plus a harmony-xml variant;
  - Kimi K2 (`<|tool_calls_section_begin|>…<|tool_call_begin|>`) and Kimi K3 (`<|open|>/<|close|>/<|end_of_msg|>`);
  - DeepSeek V3.2/V4 (DSML tokens);
  - Gemma 4 (`<|tool_call>call:` …);
  - MiniMax M3 (`<invoke name=…>`);
  - Qwen3-Coder XML (`<tool_call>\n<function=…>\n<parameter=…>`), "also used by Nemotron Nano 3, Qwen3.5 and StepFun-3.5-Flash";
  - LFM2, Cohere2MoE, GigaChat3, MiniCPM5, Ling3 (`<arg_key>`), functionary v3.2 and others.
  - Everything else, including Hermes/Qwen2.5/Qwen3 `<tool_call>{json}</tool_call>`, Llama 3.x, GLM-4.6/4.7, Granite 3.3/4.x, SmolLM3, Mistral Small 3.2 and DeepSeek V3.1/R1, goes through the autoparser.
  - Sources: [MASTER common/chat.cpp](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/chat.cpp); [MASTER models/templates/](https://github.com/ggml-org/llama.cpp/tree/339f3242c761f16c27e5ffcde1d35dcc31933a55/models/templates).
- **Ollama parser registry** (`ParserForName`): qwen3, qwen3-thinking, qwen3.5, qwen3-coder, qwen3-vl-instruct/thinking, ministral, passthrough, harmony, cogito, deepseek3, olmo3, olmo3-think, nemotron-3-nano/3.5-nano, functiongemma, glm-4.7, gemma4, gemma4-no-thinking, glm-ocr, lfm2, lfm2-thinking, laguna, poolside-v1, cohere and glimmer. The 20 non-test parser files total 7,599 lines. Example sizes:
  - qwen3coder.go 471, qwen35.go 276, gemma4.go 864, deepseek3.go 319, ministral.go 359 and glm47.go 22 (it reuses another parser).
  - Sources: [ollama model/parsers/parsers.go](https://github.com/ollama/ollama/blob/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/parsers/parsers.go); [model/parsers/](https://github.com/ollama/ollama/tree/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/parsers).
- **Ollama renderer registry** has about 22 non-test files and 5,099 lines: qwen3-coder, qwen3-vl, qwen3.5, cogito, deepseek3.1, olmo3, nemotron-3-nano, gemma4, functiongemma, glm-4.7, glm-ocr, lfm2, laguna, poolside-v1, cohere, glimmer and others. Examples: gemma4.go 842, qwen35.go 368, deepseek3.go 170 and glm47.go 179 — [ollama model/renderers/renderer.go](https://github.com/ollama/ollama/blob/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/renderers/renderer.go).
- **mistral.rs `ToolCallFormat`** has Qwen (`<tool_call>…</tool_call>`), Gemma4 (`<|tool_call>…<tool_call|>`), DeepSeek, MistralNemo (`[TOOL_CALLS]`), Hunyuan, Llama (`<|python_tag|>`), Liquid, Atem and Harmony. That is 10 files and 3,700 lines in `mistralrs-core/src/tools/parsers/`. Each parser provides `could_be_tool_call`, `parse`, `tool_call_grammar` and `required_tool_call_grammar` — [mistral.rs tools/parsers/mod.rs](https://github.com/EricLBuehler/mistral.rs/blob/4c56e256dea8fa61f7457aedacc6ce5a96a4ffdd/mistralrs-core/src/tools/parsers/mod.rs).
- **vLLM** has 53 `*_tool_parser.py` files and 38 `*_reasoning_parser.py` files. Among them are hermes, llama, llama4_pythonic, pythonic, mistral, gptoss, deepseekv3/v31/v32/v4/v41, glm47_moe, kimi_k2/k3, granite/granite4/granite_20b_fc, qwen3 (engine), minimax_m2/m3, gemma4, functiongemma, olmo3, phi4mini, xlam, step3/3p5/5 and seed_oss. The vllm/tool_parsers/*.py files total 15,383 lines.
  - New parsers are now meant to be a unified `Parser` in `vllm/parser/`, with thin tool-parser adapters on top. Constrained tool output goes through xgrammar "structural tags".
  - Sources: [vllm/tool_parsers/AGENTS.md](https://github.com/vllm-project/vllm/blob/7e3d55956577605838e9babcba84dce354b0b78c/vllm/tool_parsers/AGENTS.md); [vllm/tool_parsers/](https://github.com/vllm-project/vllm/tree/7e3d55956577605838e9babcba84dce354b0b78c/vllm/tool_parsers); [vllm/reasoning/](https://github.com/vllm-project/vllm/tree/7e3d55956577605838e9babcba84dce354b0b78c/vllm/reasoning).
- **Newer Mistral models use `[TOOL_CALLS]func_name[ARGS]{"arg": "value"}`**, not the older JSON-array form. vLLM's Mistral parser is meant to be used with mistral_common or a provided Jinja template — [vLLM mistral_tool_parser docs](https://docs.vllm.ai/en/stable/api/vllm/tool_parsers/mistral_tool_parser/).

### Inferences
Proposed Onyx parser set (one row per wire format; families from the sources above):

| # | Format | Families | Notes |
|---|---|---|---|
| 1 | Tagged JSON `<tool_call>{"name","arguments"}</tool_call>` | Hermes, Qwen2.5/Qwen3, SmolLM3, Granite 4.x (I believe; verify per template), many fine-tunes | Can be parameterised by open/close tag. This is also the "generic" fallback. |
| 2 | Qwen3-Coder XML `<function=…><parameter=…>` | Qwen3-Coder, Qwen3.5, Nemotron 3 Nano, StepFun 3.5 | Args are untyped text and must be coerced to JSON using the tool schema. |
| 3 | Llama 3.x bare JSON `{"name","parameters"}` plus `<|python_tag|>` | Llama 3.1/3.2/3.3 | Llama 4 pythonic would be a separate row if needed. |
| 4 | Mistral `[TOOL_CALLS]` | Mistral Nemo / Small 3.2 (JSON array); Ministral 3 / Devstral 2 / Mistral 3 (`name[ARGS]{…}`, optional `[CALL_ID]`) | Two sub-variants. |
| 5 | Harmony channels | gpt-oss | `<|channel|>analysis/commentary/final`, `to=functions.x`, `<|constrain|>json`; reasoning is the analysis channel. |
| 6 | DeepSeek | V3/R1/V3.1 (`<｜tool▁calls▁begin｜>…`); V3.2/V4 DSML | Two sub-variants. |
| 7 | GLM `<tool_call>name<arg_key>k</arg_key><arg_value>v</arg_value>` | GLM-4.5/4.6/4.7; Ling3 is similar | |
| 8 | Kimi | K2 section/begin tokens; K3 open/close | Two sub-variants. |
| 9 | Gemma 4 `<|tool_call>call:name{…}<tool_call|>` | Gemma 4 | Non-JSON argument syntax. Gemma 3 has no native tool tokens and needs only content parsing. |
| 10 (optional) | MiniMax M2/M3 `<invoke name=…>` | MiniMax | |
| 10 (optional) | Cohere | Cohere | |
| 10 (optional) | LFM2 | LFM2 | |

Reasoning modes:
- (a) `<think>…</think>` tags, including the variant where the generation prompt already ends with `<think>` (DeepSeek-R1, Qwen3 thinking, GLM, Kimi K2-Thinking, MiniMax). Onyx's `build_reasoning_budget` already detects this case textually.
- (b) Mistral `[THINK]…[/THINK]` (Magistral / Ministral-Reasoning). This is my inference from template names and was not verified line by line.
- (c) Harmony analysis channel.
- (d) Gemma 4 thinking channel (not verified).

The count is therefore about 9 core parsers with 3 sub-variants, and about 4 reasoning modes. Each is about 150-450 lines of C++ if written as a streaming state machine (Ollama's Go sizes are a fair proxy), or about 2.5-4k lines in total. A further 1-2k lines of tests with captured real model outputs are needed per family set.

### Gaps
- I did not verify the exact Granite 4.x, Gemma 4 thinking or Mistral `[THINK]` wire formats against the template files line by line in this pass. The templates are in `models/templates/` (ibm-granite-granite-4.0/4.1.jinja, google-gemma-4-31B-it.jinja, mistralai-Ministral-3-14B-Reasoning-2512.jinja) for the implementer to confirm.
- I found no source ranking which 2026 models are most used locally. "Popular" here means the families named in the brief plus those every project supports.

## Q4. How other projects own this layer, and which approach is less work and more robust

### Takeaway
There are two architectures:
- **Generic template plus inferred parser.** llama.cpp's Jinja engine plus autoparser handles unknown templates automatically, at the cost of about 17k complex lines and frequent API churn.
- **Explicit per-family registry.** Ollama renderers plus parsers, vLLM's named parsers and mistral.rs's `ToolCallFormat` are each simple and testable, but every new model needs new code, and the right parser must be selected.

For Onyx the robust low-effort hybrid is: keep the GGUF Jinja template for prompt rendering (using a vendored jinja/), and own a small explicit registry of output parsers and grammars, chosen by template sniffing with a CLI override. This is the vLLM/mistral.rs shape. Do not copy Ollama's renderers.

### Cited findings
- **Ollama.** Go renderers and parsers are selected by name, for example `case "qwen3.5", "tev1"` and `case "gemma4", "gemma4-small"`. They are wired through the model's Modelfile/config rather than being inferred from a template ([renderer.go](https://github.com/ollama/ollama/blob/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/renderers/renderer.go); [parsers.go](https://github.com/ollama/ollama/blob/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/parsers/parsers.go)).
  - The parser interface includes `HasToolSupport`, `HasThinkingSupport`, `ThinkingClose`, `PreservedTokens` and `Init(tools, lastMessage, thinkValue)` ([qwen3coder.go](https://github.com/ollama/ollama/blob/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/parsers/qwen3coder.go)).
  - Its streaming tool-call blog says the parser references each model's template to find the tool-call prefix, so it can stream normal text while still detecting calls ([Ollama blog: streaming tool calling](https://registry.ollama.ai/blog/streaming-tool)).
- **vLLM.** The user picks `--tool-call-parser <name>` and `--reasoning-parser`. `ParserManager.get_parser` composes the two into a unified `Parser`. Renames of parser names are treated as breaking ([AGENTS.md](https://github.com/vllm-project/vllm/blob/7e3d55956577605838e9babcba84dce354b0b78c/vllm/tool_parsers/AGENTS.md)). Prompts are rendered with HF Jinja via transformers. I did not fetch a source for that, so it is my background knowledge.
- **mistral.rs.** It renders with minijinja and uses a fixed enum of 9 tool formats. Each format supplies its own grammar for `tool_choice` auto/required ([mod.rs](https://github.com/EricLBuehler/mistral.rs/blob/4c56e256dea8fa61f7457aedacc6ce5a96a4ffdd/mistralrs-core/src/tools/parsers/mod.rs)).
- **llama.cpp.** It uses Jinja plus detection, about 20 specialised parsers and the autoparser for the rest ([MASTER common/chat.cpp](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/chat.cpp)). A community model card notes that recent llama.cpp builds "auto-derive the tool-call parser from the template", while older builds fall back to a generic Hermes-JSON parser, so tool calls show up as plain `<function=…>` text ([HF: Qwen agentic chat template for llama.cpp](https://huggingface.co/Moore2877/Qwen-Agentic-Chat-Template-llamacpp)).
- **LM Studio.** I found no primary documentation on how LM Studio renders templates or parses tool calls; it is closed source. Search results only covered llama.cpp and vLLM ([search: Liquid AI llama.cpp function-calling doc](https://docs.liquid.ai/deployment/on-device/llama-cpp/function-calling.md)).

### Inferences
- **Hand-written renderers (Ollama) are the most work and the most fragile for Onyx.** Gemma 4 alone is about 840 lines of Go. Renderers also duplicate what the GGUF template already encodes. They break `--chat-template` overrides and `chat_template_kwargs` such as `enable_thinking`, which Onyx forwards today (server.cpp L534-L538). Ollama does this because it controls its own model registry, and Onyx does not.
- **A parser registry selected by sniffing is less work than the autoparser and more transparent.** Each parser is a self-contained state machine with captured-output tests. The downside is that an unknown template yields content-only output with no tool calls. Mitigations are a generic tagged-JSON fallback, a `--tool-parser <name>` CLI flag (vLLM-style) and a clear warning.
- **Robustness ranking for Onyx's goals (few dependencies, stable across bumps):**
  1. Hybrid: vendored Jinja, owned parser registry, owned grammar builders.
  2. Keep upstream common/chat behind an adapter.
  3. Ollama-style renderers.
  - On day-one coverage of brand-new models, upstream's autoparser wins.

### Gaps
- I did not inspect how Ollama binds a GGUF/HF model to a renderer name, whether by Modelfile `RENDERER`/`PARSER` directives or by config. I inferred it from the switch statements only.

## Q5. Streaming: safe incremental deltas, and lazy grammar triggers via llama.h

### Takeaway
Upstream's streaming re-parses the entire accumulated output as "partial" on every token and diffs the parsed message against the previous one. This is simple and correct by construction, but O(n²) and brittle: it throws if the tool-call count shrinks. An Onyx-owned layer should use an incremental per-family state machine. The machine holds back any suffix that could be the start of a delimiter, streams content and reasoning immediately, and streams tool-call arguments only once they can be emitted append-only.

Lazy grammars need only llama.h: `llama_sampler_init_grammar_lazy_patterns(vocab, gbnf, root, patterns, n, tokens, n)`. The hard part is producing the GBNF, a per-format wrapper plus JSON-schema-to-GBNF for each tool's parameters. That is the piece most tied to common/ today.

### Cited findings
- **`common_chat_msg_diff::compute_diffs(prev, new)`.** It emits `reasoning_content_delta` and `content_delta` via `string_diff`. It allows a tool name to grow from a prefix and appends to the arguments. It throws "Invalid diff: now finding less tool calls!" if a re-parse yields fewer calls. The source has the comment "TODO: these can become expensive for long messages - how to optimize?" — [PIN common/chat.cpp L266-L320](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/chat.cpp).
- **Master moved to an incremental session.** `common_chat_session::feed(const common_chat_input & chunk)` / `finish()` keep state across chunks, and the input carries token IDs aligned to bytes — [MASTER common/chat.h L396-L424](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/chat.h); [PR #29876](https://github.com/ggml-org/llama.cpp/pull/29876); [PR #30210](https://github.com/ggml-org/llama.cpp/pull/30210).
- **Ollama's parsers are incremental state machines.** For example Qwen3-Coder uses `LookingForToolStart` / `CollectingToolContent` with an accumulator. Helpers `overlap(s, delim)` return the longest suffix of `s` that is a prefix of the delimiter, and `trailingWhitespaceLen` handles held-back whitespace — [ollama model/parsers/qwen3coder.go](https://github.com/ollama/ollama/blob/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/parsers/qwen3coder.go); [parsers.go L157-L170](https://github.com/ollama/ollama/blob/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/parsers/parsers.go).
- **llama.h lazy grammar API at the pin.** `llama_sampler_init_grammar_lazy_patterns(vocab, grammar_str, grammar_root, trigger_patterns, num_trigger_patterns, trigger_tokens, num_trigger_tokens)`. "Pattern will be matched from the start of the generation output, and grammar sampler will be fed content starting from its first match group". Trigger tokens feed the grammar "starting from the trigger token included". The older `llama_sampler_init_grammar_lazy` is DEPRECATED — [PIN include/llama.h L1420-L1450](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/include/llama.h).
- **How common/ converts triggers to that API.**
  - WORD triggers become regex-escaped patterns. PATTERN is passed through. PATTERN_FULL is anchored `^…$`. TOKEN goes into `trigger_tokens`.
  - It then calls `llama_sampler_init_grammar_lazy_patterns(...)`, or `llama_sampler_init_grammar` when not lazy.
  - Source: [PIN common/sampling.cpp L220-L271](https://github.com/ggml-org/llama.cpp/blob/9723942adc518b43c4b95dc4dce6906903eb5e09/common/sampling.cpp).
  - Onyx already promotes single-token WORD triggers to TOKEN triggers when the token is preserved ([src/server.cpp L414-L428](../../src/server.cpp)).
- **Grammar text comes from the PEG parser plus JSON-schema conversion.** Upstream derives it from the PEG arena (`data.parser.build_grammar(builder, data.grammar_lazy)`) and from json-schema-to-grammar.cpp, 1,264 lines at the pin ([MASTER common/parsers/qwen3-coder.cpp L180](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/parsers/qwen3-coder.cpp)). mistral.rs likewise gives each format a `tool_call_grammar` and a `required_tool_call_grammar` ([mod.rs L88-L91](https://github.com/EricLBuehler/mistral.rs/blob/4c56e256dea8fa61f7457aedacc6ce5a96a4ffdd/mistralrs-core/src/tools/parsers/mod.rs)).

### Inferences
Design for the Onyx streaming parser (`onyx::chat::parser` interface):

```cpp
struct onyx_chat_delta { std::string reasoning, content; int tool_index=-1;
                         std::string tool_id, tool_name, tool_args_append; };
struct onyx_output_parser {
  virtual void init(const tools&, bool thinking_open_in_prompt) = 0;
  virtual void feed(std::string_view piece, llama_token tok, std::vector<onyx_chat_delta>& out) = 0;
  virtual void finish(std::vector<onyx_chat_delta>& out, finish_info&) = 0; // flush held-back text, close/repair partial call
  virtual grammar_spec grammar(const tools&, tool_choice, bool parallel) const = 0; // GBNF + triggers + preserved tokens
  virtual reasoning_tags thinking() const = 0;   // feeds build_reasoning_budget
};
```

- **Hold-back rule.** Keep the longest suffix that is a prefix of any active delimiter (Ollama's `overlap`). Match on token IDs where delimiters are special tokens, which is what #29876 adds upstream. Hold-back is then mostly unnecessary, because `token_to_piece` with special tokens gives atomic pieces.
- **Argument streaming.**
  - For JSON-argument formats (1, 3, 4, 5, 6, 8), stream the raw argument bytes as `tool_args_append`. They are append-only by construction and OpenAI clients concatenate them.
  - For tagged-argument formats (2, 7, 9), emit each argument's JSON fragment when that parameter closes. That means `{"k":` plus the value encoded using the schema type. Close with `}` on the end tag.
  - Never retract emitted bytes. Under a grammar the output can't diverge. Without one, an invalid call at finish should surface as content plus a `finish_reason` of `stop`, not a crash. Upstream's re-parse throws in that case.
- **Lazy grammar.** For each format, triggers are its opening delimiter (TOKEN if it is a single special token, otherwise WORD/PATTERN). The grammar is `root ::= <open> call (sep call)* <close>`, with per-tool alternatives `"\"name\": \"" name "\", \"arguments\": " <schema-gbnf>`. With `tool_choice=required` or a named tool, the grammar is non-lazy, or PATTERN_FULL `^`-anchored, after any reasoning block. Thinking models need the trigger to fire after `</think>`, which upstream handles with patterns that allow a reasoning prefix.
- **JSON-schema-to-GBNF.** This is the piece with the worst replace/keep ratio. It is about 1.0-1.3k lines of fiddly code (patterns, formats, min/max items, `$ref`, additionalProperties). It is used both for `response_format` and for tool parameters. Options are to keep `json-schema-to-grammar.cpp` as a vendored single file, since it depends on common_json and a few string utils, or to write a restricted converter covering object/array/string/number/integer/boolean/enum/const/required/anyOf (about 400-600 lines) and refuse unsupported keywords under `strict`. Verdict: vendor-fork it now and consider a rewrite later.
- **Cost of re-parsing.** Upstream's re-parse-and-diff costs O(n) per token, so O(n²) per response. At 8k-token outputs this is measurable but small next to decode time. Correctness, not speed, is the main reason to go incremental.

### Gaps
- I did not benchmark upstream's per-token re-parse cost.
- I did not read upstream's PEG-to-GBNF generator in detail, nor how it builds the "reasoning prefix then trigger" patterns. An implementer should read `common/chat-peg-parser.cpp` and `common/peg-parser.cpp` before writing grammar builders.

## Q6. JSON library: keep nlohmann/json or replace it?

### Takeaway
Keep nlohmann/json, but take it directly rather than through `common_json`. It is single-header, MIT and v3.12.0 in the vendored tree. Upstream has wrapped it in a pimpl `common_json` class whose API deliberately differs from nlohmann and changes between pins. Onyx's server.cpp uses `common_json` about 100 times, so that wrapper is itself a common/ dependency. Vendor `json.hpp` into `third_party/nlohmann/`, apply or track upstream's deep-nesting backport (or enforce a request nesting-depth cap), and use `nlohmann::ordered_json`. Replacing JSON entirely is not worth it.

### Cited findings
- **Where it comes from.** `vendor/nlohmann/json.hpp` is 25,526 lines at the pin and 26,367 on master, version 3.12.0, plus json_fwd.hpp at 187 lines ([PIN vendor/nlohmann/](https://github.com/ggml-org/llama.cpp/tree/9723942adc518b43c4b95dc4dce6906903eb5e09/vendor/nlohmann); [MASTER vendor/nlohmann/](https://github.com/ggml-org/llama.cpp/tree/339f3242c761f16c27e5ffcde1d35dcc31933a55/vendor/nlohmann)).
- **Upstream's deep-nesting patch.** Master adds `json-deep-nesting.patch`, 1,184 lines: a "Backport of the nlohmann/json fixes for the stack overflow on deeply nested values (nlohmann/json#5387) onto v3.12.0". Copying, `dump()` and comparing a deeply nested value "would crash with SIGSEGV". Upstream marks it to be removed once nlohmann releases a version newer than 3.12.0 — [MASTER vendor/nlohmann/json-deep-nesting.patch](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/vendor/nlohmann/json-deep-nesting.patch).
- **`common_json` is a thin pimpl wrapper.** Its header says: "the underlay library is pimpl, we are using nlohmann::json for now". It has insertion-ordered keys, throws `common_json_error`, and "obj.push_back({key, val}) is intentionally unsupported". It is 352 lines at the pin and 356 on master, with common/json.cpp at 433 lines — [MASTER common/json.h](https://github.com/ggml-org/llama.cpp/blob/339f3242c761f16c27e5ffcde1d35dcc31933a55/common/json.h).
- **Onyx usage.** Onyx uses `common_json` and `common_json_error` throughout server.cpp: response building, request parsing and SSE chunks ([src/server.cpp](../../src/server.cpp), e.g. L33-L40, L89-L211, L621).
- **minja requires `nlohmann::ordered_json`** ([google/minja README](https://github.com/google/minja/blob/021c2293c187789ef13d56c6cfd89c9b134fd80f/README.md)). The upstream Jinja engine needs JSON only for `global_from_json` (value.cpp/caps.cpp).

### Inferences
- Writing an Onyx JSON library (parser, serializer, ordered object, number handling, UTF-8 validation, partial-JSON helpers for streaming) is about 1.5-2.5k lines and 1-2 weeks, and it would be less battle-tested. nlohmann is header-only and adds no link-time dependency, so the "fewer dependencies" goal is already met by vendoring one header.
- The only cost is compile time: json.hpp is about 26k lines. Mitigate it by including nlohmann only in a few .cpp files, plus json_fwd.hpp where only declarations are needed.
- The security note matters for Onyx's threat model, since it accepts untrusted request bodies. Either carry upstream's patch, or reject bodies whose nesting depth exceeds, say, 64 before calling `parse`. A depth-counting pre-scan is about 30 lines. nlohmann's parser also accepts a `parser_callback_t` that can reject by depth.
- Migrating from `common_json` to `nlohmann::ordered_json` in server.cpp is mostly mechanical: `common_json::object()` maps to `ordered_json::object()`, and the error type maps to `nlohmann::json::exception`. Estimate about 0.5-1 day.

### Gaps
- I did not check whether nlohmann/json has released a version after 3.12.0 as of October 2026. If it has, the patch can be dropped.

## Q7. Effort estimate, quality risk and replace-vs-keep verdict per component

### Takeaway
Replace or own the thin, churny glue: the OpenAI message/tool parsing, the streaming delta state machine, the format registry, the tool-call parsers and the lazy-grammar wrappers. Vendor-fork the hard, stable, generic pieces: the Jinja engine, JSON-schema-to-GBNF and nlohmann. Do not rewrite the Jinja engine or the autoparser. This cuts all `common_chat_*` calls out of Onyx, so pin bumps stop breaking the chat layer. It takes about 4-7 engineer-weeks with quality at least equal for the supported families. The honest loss is automatic support for unknown, novel templates, which upstream's autoparser provides.

### Cited findings
All of these were established above:
- the sizes of upstream's components (Q1);
- that the Jinja engine is stable and the chat API churns ([PR #30210](https://github.com/ggml-org/llama.cpp/pull/30210), [PR #29876](https://github.com/ggml-org/llama.cpp/pull/29876));
- that peer projects ship 9-53 named parsers (Q3);
- that upstream's own Jinja engine still misses 14 of 370 templates after 137 commits ([PR #18462](https://github.com/ggml-org/llama.cpp/pull/18462));
- Ollama's per-family parser sizes of 22-864 lines of Go ([model/parsers/](https://github.com/ollama/ollama/tree/2cd63f7a15039ccd790e97b31edc091fa1707c56/model/parsers)).

### Inferences
Effort and risk are my estimates for one experienced C++ engineer, informed by the line counts above.

| Component | Upstream today (pin) | Verdict | Onyx approach | Effort | Quality risk |
|---|---|---|---|---|---|
| Jinja engine | common/jinja, 6,349 lines | **Keep the code, own the copy** | Vendor-fork into `src/chat/jinja/`; swap common_json for nlohmann in 3 files; port jinja golden tests; cherry-pick fixes | 2-4 days | Low: same engine as today |
| Jinja: write own subset | — | **Do not** | — | 3-6 weeks | High: upstream needed 137 commits for 356/370 templates |
| Jinja: minja | 3,627 lines, last google commit 2025-09 | Fallback only | — | 1-2 days | Medium: orphaned, no input marking |
| Template application glue (bos/eos, `add_generation_prompt`, `enable_thinking`, kwargs, tools JSON, per-family message workarounds such as Gemma 4 tool responses) | inside chat.cpp | **Replace** | `onyx_chat_render()` about 300-500 lines; copy the specific workarounds per family as needed | 3-5 days | Medium: workarounds hide in chat.cpp and parsers/*.cpp and must be audited per supported family |
| OpenAI msgs/tools/tool_choice parsing (`*_parse_oaicompat`) | chat.cpp | **Replace** | About 200-300 lines on nlohmann | 1-2 days | Low |
| Format detection | substring sniffing, about 150 lines | **Replace** | Same technique, Onyx-owned table, plus a `--tool-parser` override and a warning on unknown | 1 day | Low |
| Per-family output parsers (about 9 formats plus 3 sub-variants) | parsers/ (3,360 master) plus PEG engine (2,654) | **Replace** | Hand-written streaming state machines, about 2.5-4k lines in total, with captured-output fixtures from `models/templates/` and real runs | 2-3 weeks | Medium: edge cases such as partial special tokens, parallel calls, XML arg typing and Gemma 4 syntax. Mitigate with the upstream test-chat.cpp cases as fixtures. |
| Autoparser (diff analyzer) | about 3,000 lines | **Do not replicate** | Generic tagged-JSON fallback plus explicit registry | — | Accepted loss: unknown templates get content-only output |
| Reasoning parsing | in each handler | **Replace** | Shared `<think>` / `[THINK]` / harmony-channel components, about 300 lines; also feeds `build_reasoning_budget` | 2-3 days | Low |
| Streaming delta emission (msg_diff) | chat.cpp | **Replace** | Incremental `feed()` → delta vector; no re-parse | Included in parsers | Low-medium; more robust than re-parse-and-diff |
| Lazy grammar wrappers per format | PEG→GBNF | **Replace** | Hand-written GBNF templates per format plus triggers; call `llama_sampler_init_grammar_lazy_patterns` directly once sampling is owned | 3-5 days | Medium: GBNF/trigger interplay with thinking prefixes |
| JSON-schema → GBNF | json-schema-to-grammar.cpp, 1,264 lines | **Keep (vendor-fork)** for now | Copy the single file, decouple from common_json; maybe a restricted rewrite later | 1-2 days | Low (keep); High if rewritten hastily |
| JSON library | nlohmann 3.12.0 via common_json | **Keep nlohmann, drop common_json** | Vendor json.hpp, use `ordered_json`, add a nesting-depth cap or carry the upstream patch | 0.5-1 day plus a mechanical migration | Low |
| Tests | upstream 13,353 lines of chat tests | **Must build** | Golden prompt renders for about 20 reference templates; captured outputs per parser, streamed token by token and checked against non-streamed | 1-1.5 weeks | — |

- **Total for the hybrid:** about 4-7 weeks. A useful early milestone (about 2 weeks) is the vendored Jinja, own message parsing, `<think>` reasoning, tagged-JSON (Hermes/Qwen3), Qwen3-Coder XML, harmony and Llama 3, while still delegating the other families to upstream.
- **Staging.** Introduce `src/chat/onyx_chat.h` as the single seam the earlier report recommends. Implement it first over upstream `common_chat_session` (master API), then replace the implementation family by family behind it. Keep a `--chat-backend=upstream` switch until golden tests pass for each family.
- **Alternative: keep upstream behind the adapter.** It takes about 3-5 days to build the adapter and about 0.5-2 days per pin bump to absorb churn such as #30210. Quality and coverage are best, with automatic support for new models, but the dependency stays. This is the right choice if the owner values day-one support for new model families over fewer dependencies.
- **Hidden coupling to note.** `common_params_sampling.grammar_triggers`, `preserved_tokens` and `generation_prompt` flow into `common_sampler`. Owning the chat layer's grammar output pays off fully only if Onyx also owns sampler-chain construction over llama.h (`llama_sampler_chain_*`, `llama_sampler_init_grammar_lazy_patterns`), which is covered by the sampling research. Until then, the Onyx parser can fill `common_params_sampling` fields directly.

### Gaps
- The effort numbers are engineering estimates, not measured; no source gives person-week costs for building these layers.
- I did not audit every per-family message workaround upstream applies before rendering, such as tool-response reshaping for Gemma 4, Mistral tool-call-ID constraints and DeepSeek reasoning re-insertion. That list determines how much render glue Onyx must copy.
