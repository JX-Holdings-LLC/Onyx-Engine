# API Surfaces and Interoperability Standards for a Local OpenAI-Compatible Server (Oct 2026)

Baseline (from repo docs/source): Onyx exposes `/v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, `/v1/models`, `/health`, `/props`, `/tokenize`, `/detokenize`, `/apply-template`. Streams `reasoning_content` deltas; `/health` is always `{"status":"ok"}` because the server only listens after load (docs/api.md). `/v1/embeddings` rejects `dimensions` and any `encoding_format` other than `float` with 400 (src/server.cpp:1193-1198). `--pooling rank` rerankers return raw scores through `/v1/embeddings`, with no dedicated rerank route (docs/api.md; src/engine.cpp:472). The docs do not list a top-level `reasoning_effort` or `enable_thinking` field; thinking is controlled only via `chat_template_kwargs` and `reasoning_budget_tokens`. CORS is denied by default, `/v1/models` returns a minimal `meta`, there is no `/metrics`, and the docs mention no request ID header.

Method note: the most reliable source for llama.cpp's current surface was its `master` source code, `tools/server/server.cpp` (route table, fetched 2026-10-21 via raw.githubusercontent.com). The rendered README was truncated in fetch. The llama.cpp source and the vLLM docs both list a `/v1/systemone` route, and the LM Studio docs list a "Decisions"/"System One" endpoint. I could not find what these are, so they are excluded from recommendations.

## 1. OpenAI Responses API (/v1/responses) and Open Responses

### Takeaway
By late 2026, `/v1/responses` is a table-stakes second API surface. llama.cpp, vLLM, Ollama and LM Studio all ship it, and the vendor-neutral "Open Responses" spec (Jan 2026) standardizes it. Local servers mostly implement a **stateless** subset by converting Responses into Chat Completions internally. That is the minimum viable path for Onyx: reuse the chat pipeline, emit the Responses SSE event state machine, and reject `previous_response_id` with a clear error, or support it with an in-memory cache.

### Cited Findings
- Open Responses is an open spec built on the OpenAI Responses API. It was announced 2026-01-15, with launch partners OpenRouter, Hugging Face, LM Studio, vLLM, Ollama and Vercel. It ships server-side acceptance/conformance tests — [openresponses.org](https://openresponses.org/); [Simon Willison 2026-01-15](https://simonwillison.net/2026/Jan/15/open-responses/)
- Spec (rev. 2026-04-24), transport: `POST /v1/responses`, with `/responses/compact` mentioned for compaction. In SSE, the `event:` name MUST equal the body `type`. The stream MUST end with `response.completed`, `response.failed` or `response.incomplete`; `[DONE]` is optional. Every event carries a monotonically increasing `sequence_number`. A WebSocket mode (`{"type":"response.create"}`) is optional for servers — [Open Responses spec](https://www.openresponses.org/specification)
- Spec, item model: core items are `message`, `function_call` and `reasoning`, and content types include `input_text` and `output_text`. Each item needs `id`, `type` and `status` (`in_progress` | `incomplete` | `completed`). Per-item event order: `response.output_item.added` → `response.content_part.added` → deltas (`response.output_text.delta`, `response.function_call_arguments.delta`) → `*.done` → `response.content_part.done` → `response.output_item.done`. Response-level events are `response.in_progress`, `response.completed`, `response.failed` and `response.incomplete` — [Open Responses spec](https://www.openresponses.org/specification)
- Spec, state: `previous_response_id` MUST load the prior input and output. With `store=false` and nothing cached, the server MUST fail with `previous_response_not_found`. Reasoning items have optional `content` (raw trace), `encrypted_content` (opaque, to be round-tripped) and `summary`. `tool_choice` accepts `auto|required|none|{type:function,name}|{type:allowed_tools,tools:[...]}`, and `allowed_tools` MUST be enforced as a hard constraint. Error types are `server_error`/`model_error` (500), `invalid_request` (400), `not_found` (404) and `too_many_requests` (429). Extensions must use a slug prefix (e.g. `acme:trace_event`), and clients ignore unknown types — [Open Responses spec](https://www.openresponses.org/specification)
- llama.cpp master registers `POST /v1/responses` and `/responses`, plus `POST /v1/responses/input_tokens` (token counting). The handler calls `server_chat_convert_responses_to_chatcmpl()`, i.e. it converts the request to Chat Completions — [llama.cpp server.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server.cpp); [server-context.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-context.cpp)
- llama.cpp throws `"llama.cpp does not support 'previous_response_id'."` and rejects `input_file`. It maps `reasoning` input items to `reasoning_content` on the converted chat message, and maps request `reasoning` into chat params — [server-chat.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-chat.cpp)
- llama.cpp's Responses stream emits `response.created`, `response.in_progress`, `response.output_item.added`, `response.reasoning_text.delta`, `response.output_item.done` and `response.completed` — [server-task.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-task.cpp)
- Earlier history: issue #19138 reported that llama-server lacked `/v1/responses`. PR #21174, "improve Responses API compliance and Codex CLI compatibility" (April 2026), shows Codex CLI is a driver of this work — [issue #19138](https://github.com/ggml-org/llama.cpp/issues/19138); [PR #21174 diff view](https://app.semanticdiff.com/gh/ggml-org/llama.cpp/pull/21174/overview)
- vLLM lists `/v1/responses`, `/v1/responses/{response_id}` and `/v1/responses/{response_id}/cancel`, plus `/v1/responses/render` in scale-out mode — [vLLM online serving docs](https://docs.vllm.ai/en/latest/serving/online_serving/)
- Ollama added `/v1/responses` in v0.13.3. It supports streaming, function tools, reasoning summaries, `instructions`, `max_output_tokens`, `reasoning.effort` and an Ollama `think` extension. It does **not** support `previous_response_id`, `conversation`, `truncation` or stateful requests ("only the non-stateful flavor") — [Ollama OpenAI compatibility](https://docs.ollama.com/api/openai-compatibility)
- LM Studio's developer docs list "Responses" among its OpenAI-compatible endpoints, alongside a native REST API with stateful chats — [LM Studio developer docs](https://lmstudio.ai/docs/developer)
- Open WebUI describes its Open Responses support as experimental — [Open WebUI docs](https://docs.openwebui.com/getting-started/quick-start/connect-a-provider/starting-with-open-responses)

### Inferences
- Minimum viable Responses subset for Onyx:
  - Request fields: `input` (string or item array: `message`, `function_call`, `function_call_output`, `reasoning`), `instructions`, `tools` (flat `{type:"function",name,parameters,strict}`), `tool_choice`, `max_output_tokens`, `temperature`/`top_p`, `stream`, `text.format` (json_schema), `reasoning.effort`.
  - Behavior: convert to the existing chat path, as llama.cpp does.
  - Streaming: emit the full item/content-part event state machine with `sequence_number` and the `event:` line.
  - State: return 400 `previous_response_not_found`, or accept `previous_response_id` from a small in-process LRU so Codex-style clients work. Stateful storage is optional for local servers in practice (Ollama and llama.cpp skip it).
- Add `POST /v1/responses/input_tokens` cheaply. Onyx already has tokenize plus the template path.
- Run the Open Responses acceptance tests as a CI conformance gate.

### Gaps
- I did not confirm whether llama.cpp PR #21174 merged. Current master already has the Responses route either way.
- I did not fetch the exact LM Studio version or date for Responses support, or whether LM Studio supports `previous_response_id` (its changelog was not fetched).
- I did not review the Open Responses acceptance-test contents or harness URL (the `/compliance` page).

## 2. Anthropic Messages API (/v1/messages) in local servers

### Takeaway
Anthropic Messages compatibility became standard among local servers in 2026. llama.cpp (Jan 2026), Ollama and vLLM all expose `/v1/messages`, and llama.cpp and vLLM also expose `/v1/messages/count_tokens`. The main driver is Claude Code and other Anthropic-SDK agents, which only need `ANTHROPIC_BASE_URL` pointed at the server. For Onyx this is a translation layer onto the chat pipeline plus Anthropic-shaped SSE.

### Cited Findings
- llama.cpp added an Anthropic Messages API (PR #17570), announced 2026-01-19. It covers SSE streaming with Anthropic event types, `POST /v1/messages/count_tokens`, `tool_use`/`tool_result` blocks, vision, and the `thinking` parameter. It is implemented by converting Anthropic requests to OpenAI format internally. The Claude Code setup is `ANTHROPIC_BASE_URL=http://127.0.0.1:8080 claude` — [HF blog: Anthropic Messages API in llama.cpp](https://huggingface.co/blog/ggml-org/anthropic-messages-api-in-llamacpp)
- llama.cpp master registers `POST /v1/messages` and `POST /v1/messages/count_tokens` — [server.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server.cpp)
- In llama.cpp's converter, Anthropic `thinking` blocks are concatenated into `reasoning_content` on the converted assistant message — [server-chat.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-chat.cpp)
- vLLM lists Anthropic `/v1/messages` and `/v1/messages/count_tokens` — [vLLM online serving docs](https://docs.vllm.ai/en/latest/serving/online_serving/). The earlier feature request was explicitly motivated by Claude Code — [vLLM issue #21313](https://github.com/vllm-project/vllm/issues/21313)
- Ollama, supported: `/v1/messages`, multi-turn, system prompts, streaming, tools and tool results, `thinking`, `output_config.effort`, base64 images, `temperature`/`top_p`/`top_k`/`stop_sequences`.
- Ollama, unsupported: `tool_choice`, `count_tokens`, `cache_control` prompt caching, Batches, citations, PDF `document` blocks and `metadata`. Mid-stream errors are not sent as `error` events, and `budget_tokens` is accepted but not enforced.
- Ollama, Claude Code setup: `ANTHROPIC_AUTH_TOKEN=ollama` and `ANTHROPIC_BASE_URL=http://localhost:11434`. The cloud endpoint requires `anthropic-version: 2023-06-01` — [Ollama Anthropic compatibility](https://docs.ollama.com/api/anthropic-compatibility)
- LM Studio lists an "Anthropic-compatible: Messages" endpoint — [LM Studio developer docs](https://lmstudio.ai/docs/developer)
- Anthropic stream order: `message_start` (Message with empty `content`) → per block `content_block_start` → `content_block_delta`* → `content_block_stop` → `message_delta` → `message_stop`. `ping` events may appear anywhere. SSE `event:` names match the data `type` — [Anthropic streaming docs](https://platform.claude.com/docs/en/build-with-claude/streaming)
- Anthropic delta types: `text_delta`, `input_json_delta` (`partial_json` chunks for tool input), and `thinking_delta`, followed by a `signature_delta` just before `content_block_stop` for thinking blocks. Mid-stream errors use `event: error` with `{"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}}` — [Anthropic streaming docs](https://platform.claude.com/docs/en/build-with-claude/streaming)

### Inferences
- MVP for Onyx:
  - Request fields: `model`, `system` (string or blocks), `messages` with `text`/`image`(base64)/`tool_use`/`tool_result`/`thinking` blocks, `max_tokens` (required in Anthropic), `stop_sequences`, `temperature`/`top_p`/`top_k`, `tools` (`input_schema`), `tool_choice` (`auto`/`any`/`tool`/`none`), and `thinking: {type, budget_tokens}`. Map `budget_tokens` onto Onyx's existing reasoning-budget machinery, which enforces it. Ollama does not, so this is a differentiator.
  - Response: `stop_reason` mapping (`end_turn`/`max_tokens`/`stop_sequence`/`tool_use`) and `usage.input_tokens`/`output_tokens`.
  - Count tokens: `/v1/messages/count_tokens`.
  - Auth: accept `x-api-key` as well as `Authorization: Bearer`, since Anthropic SDKs send `x-api-key`. Tolerate/ignore `anthropic-version` and `anthropic-beta` headers.
- Thinking `signature` must be emitted, as an opaque placeholder string, and round-tripped harmlessly. Clients may expect a `signature_delta`.
- Behind JX Runtime, the proxy's `KIND_PATHS` would need to forward `/v1/messages*`.

### Gaps
- I did not fetch vLLM's detailed Anthropic feature matrix (which fields are unsupported).
- I did not verify exact Claude Code requirements on local servers beyond `ANTHROPIC_BASE_URL`/`ANTHROPIC_AUTH_TOKEN`. One blog commenter reported that `ANTHROPIC_MODEL` is also needed (unverified).

## 3. Reasoning content conventions

### Takeaway
There are two competing field names. `reasoning_content` comes from DeepSeek and is what llama.cpp and Onyx emit. `reasoning` comes from OpenRouter and the OpenAI gpt-oss guidance, and is what vLLM switched to. Clients increasingly read `reasoning`. Onyx should support emitting both, or a switch, and accept both on input. `reasoning_effort` (top-level, with `"none"` meaning thinking off) has become the cross-server thinking toggle, mapped onto chat-template kwargs. Interleaved thinking with tool calls requires preserving prior reasoning across tool turns, per the harmony rules.

### Cited Findings
- vLLM: "`reasoning` used to be called `reasoning_content`". Clients should migrate, because the old field may come back empty. Function calls are parsed only from `content`, not `reasoning`. Structured output is deferred until the reasoning end token. `reasoning_effort` auto-injects `enable_thinking` (`"none"` → false; `low`/`medium`/`high` → true), and explicit `enable_thinking` overrides it. Some models use other kwargs (`thinking: true` for Granite 3.2 and DeepSeek-V3.1). `--default-chat-template-kwargs` sets server defaults — [vLLM reasoning outputs](https://docs.vllm.ai/en/latest/features/reasoning_outputs/)
- llama.cpp server: `--reasoning-format none|deepseek|deepseek-legacy|auto`, where `deepseek` puts thoughts in `message.reasoning_content`. It also has `--reasoning-budget` and `--reasoning-budget-message` — [llama.cpp server README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md). Streaming emits `delta.reasoning_content` — [server-chat.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-chat.cpp)
- llama.cpp parses top-level `reasoning_effort`: `"none"` sets `enable_thinking=false`, and other values are passed into `chat_template_kwargs["reasoning_effort"]`. A string-typed `enable_thinking` is rejected — [server-common.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-common.cpp)
- Ollama accepts `reasoning_effort` and `reasoning.effort` with model-defined values. It maps `minimal` → `low` and `xhigh`/`ultra` → `max` (or `high` for gpt-oss), and `none` disables thinking — [Ollama OpenAI compatibility](https://docs.ollama.com/api/openai-compatibility)
- OpenAI gpt-oss guidance, Chat Completions: follow OpenRouter's convention, with raw CoT as a `reasoning` property on the message and on streaming deltas. `reasoning: {exclude: true}` opts out. Raw CoT should not be shown to end users.
- OpenAI gpt-oss guidance, Responses: the `reasoning` item gets a `content` array of `reasoning_text` parts, streamed via `response.reasoning_text.delta`/`.done`.
- OpenAI gpt-oss guidance, harmony channels: `analysis` holds CoT, `commentary` holds function calls, `final` holds the answer. After a `final` message, earlier `analysis` is dropped, but if the last assistant message was a tool call, the `analysis` back to the previous `final` must be kept — [OpenAI cookbook: handle raw CoT (gpt-oss)](https://developers.openai.com/cookbook/articles/gpt-oss/handle-raw-cot)
- Anthropic's equivalent is `thinking` content blocks with `thinking_delta` and `signature_delta` — [Anthropic streaming docs](https://platform.claude.com/docs/en/build-with-claude/streaming)

### Inferences
- Add a `--reasoning-field reasoning_content|reasoning|both` option, or emit both. On input, accept `reasoning`, `reasoning_content` and `reasoning_details`-less messages, and pass them to the template. This is what lets gpt-oss and Qwen3/DeepSeek templates re-render interleaved thinking between tool calls.
- Parse top-level `reasoning_effort` (and Responses `reasoning.effort`). Map `"none"` to `enable_thinking=false` plus budget 0, and other values into `chat_template_kwargs.reasoning_effort` (gpt-oss harmony templates read it). This mirrors llama.cpp and vLLM.
- Consider `reasoning: {exclude: true}` (OpenRouter style) as a request flag to strip reasoning from the response.

### Gaps
- I did not verify OpenRouter's current `reasoning`/`reasoning_details` schema directly.
- I did not confirm which major clients (Open WebUI, LibreChat, Cline) read `reasoning` vs `reasoning_content` as of Oct 2026.

## 4. Rerank, /infill FIM, audio transcription, embeddings options

### Takeaway
llama.cpp and vLLM both ship a Jina/Cohere-style `/v1/rerank`, llama.cpp's `/infill` is the de facto local FIM endpoint, and OpenAI-style `/v1/audio/transcriptions` now exists in both. Onyx already has the reranker compute path (`--pooling rank`) but no rerank route. Onyx also rejects `encoding_format: "base64"` and `dimensions`, both of which Ollama and llama.cpp accept.

### Cited Findings
- llama.cpp routes `/rerank`, `/reranking`, `/v1/rerank` and `/v1/reranking` (which require `--embedding --pooling rank`), plus `/infill`, `/v1/audio/transcriptions`, `/tts` and a `/v1/realtime` WebSocket — [server.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server.cpp); [README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
- llama.cpp `/infill`: streaming code infill from prefix and suffix with optional `input_extra` context — [README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
- llama.cpp `/v1/embeddings` accepts `encoding_format` of `float` or `base64` (other values → 400). Inputs may also be OAI content arrays with image/audio parts — [server-context.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-context.cpp)
- vLLM: Cohere-compatible `/rerank`, `/v1/rerank`, `/v2/rerank`, plus `/v2/embed`, `/score`, `/v1/score`, `/classify`, `/pooling`, `/v1/audio/transcriptions`, `/v1/audio/translations`, and `/v1/realtime` (ASR models) — [vLLM online serving docs](https://docs.vllm.ai/en/latest/serving/online_serving/)
- Ollama `/v1/embeddings` supports `encoding_format` and `dimensions` but not token-array inputs — [Ollama OpenAI compatibility](https://docs.ollama.com/api/openai-compatibility)

### Inferences
- Rerank route for Onyx: accept `{model, query, documents: [string | {text}], top_n, return_documents}`. Build the query/document pair input per the model's template (llama.cpp formats the rerank prompt from model metadata). Return `{results: [{index, relevance_score, document?}], usage}`, sorted descending. This turns an existing capability into a standard API usable by Open WebUI and LibreChat RAG rerankers.
- Embeddings:
  - Accept `encoding_format: "base64"` (little-endian float32 bytes, base64), which the OpenAI Node/Python SDKs request by default.
  - Accept `dimensions` for Matryoshka models: truncate, then re-L2-normalize. Alternatively, reject it only when `dimensions` is above `n_embd`.
- `/infill`: worth adding if Onyx targets code editors (Continue, llama.vim/llama.vscode FIM). It needs FIM tokens from the vocab (`llama_vocab_fim_pre/suf/mid`). `/v1/completions` `suffix` is the OpenAI-shaped alternative.
- Audio transcription requires a separate ASR-capable model and multipart parsing. It is low priority for a single-model text server.

### Gaps
- I did not fetch the exact llama.cpp `/infill` request schema or the Jina rerank response schema details from primary docs.
- I did not confirm whether the OpenAI SDKs' default `encoding_format: base64` behavior still holds in 2026 SDK versions (from prior knowledge, unverified here).

## 5. Tool calling conventions

### Takeaway
The OpenAI contract includes several pieces local servers must emulate faithfully. `tool_choice` options are auto/required/none/forced/`allowed_tools`. `parallel_tool_calls:false` means at most one call, and `strict: true` requires a schema subset. Streaming deltas are incremental argument strings keyed by index. Onyx already covers most Chat Completions basics via llama.cpp's common chat parser. The gaps are `allowed_tools`, honoring `strict` (Onyx's grammar machinery makes this enforceable), and the Responses/Anthropic streaming shapes.

### Cited Findings
- `tool_choice` takes `auto` (default), `required`, `none`, a forced function `{"type":"function","name":...}`, or `allowed_tools` (`mode` plus a `tools` subset; this keeps the tools list stable for prompt caching). `parallel_tool_calls:false` ensures "exactly zero or one tool is called" — [OpenAI function calling guide](https://developers.openai.com/api/docs/guides/function-calling)
- Strict mode requires `additionalProperties:false` on every object and every property listed in `required`, with optional fields expressed as a `"null"` type union. Responses normalizes to strict by default when possible, while Chat Completions is non-strict by default — [OpenAI function calling guide](https://developers.openai.com/api/docs/guides/function-calling)
- Responses tool streaming: `response.output_item.added` (item with `name`, `call_id`, `id`, empty `arguments`) → `response.function_call_arguments.delta` (`delta`, `item_id`, `output_index`) → `response.function_call_arguments.done` → `response.output_item.done`. Custom tools (`type:"custom"`) return free text, optionally constrained by a Lark or regex CFG in `format` — [OpenAI function calling guide](https://developers.openai.com/api/docs/guides/function-calling)
- vLLM: `parallel_tool_calls=false` limits output to zero or one call, and multiple calls are model-dependent — [vLLM online serving docs](https://docs.vllm.ai/en/latest/serving/online_serving/)
- llama.cpp defaults `parallel_tool_calls` to the template capability `supports_parallel_tool_calls` — [server-common.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-common.cpp)
- Ollama does not support `tool_choice` on `/v1/chat/completions` — [Ollama OpenAI compatibility](https://docs.ollama.com/api/openai-compatibility)
- Anthropic streams tool input as `input_json_delta.partial_json` — [Anthropic streaming docs](https://platform.claude.com/docs/en/build-with-claude/streaming)
- gpt-oss emits tool calls on the harmony `commentary` channel, and tool calls can occur inside CoT — [OpenAI cookbook gpt-oss](https://developers.openai.com/cookbook/articles/gpt-oss/handle-raw-cot)

### Inferences
- Add `tool_choice: {type:"allowed_tools", ...}`, implemented by filtering the grammar/trigger set while still rendering all tools in the prompt.
- When `strict: true` is set on a function, constrain the arguments with a grammar generated from its schema. This is a real reliability win over Ollama.
- Default `parallel_tool_calls` from template capabilities, as llama.cpp does.
- Return the tool-call `id`s in a consistent `call_<hex>` format, and accept `tool_call_id` on tool messages.
- Model-family differences (Hermes/Qwen `<tool_call>`, Llama 3.x JSON, Mistral `[TOOL_CALLS]`, gpt-oss harmony, DeepSeek) are handled by llama.cpp's `common_chat` parsers, so Onyx inherits them. The risk is mainly streaming partial-JSON diff quality.

### Gaps
- I did not fetch the Chat Completions streaming tool_call delta spec (`index`, `id`, `function.arguments`) from OpenAI docs (the page did not show it), nor a per-family support matrix from a primary source.

## 6. Observability: Prometheus /metrics, OpenTelemetry GenAI, request IDs

### Takeaway
Both reference servers expose Prometheus `/metrics`: llama.cpp behind `--metrics` with a `llamacpp:` prefix, and vLLM always with a `vllm:` prefix and a `model_name` label. The OpenTelemetry GenAI semconv (still "Development" stability, now in a separate repo) defines server histograms for TTFT, TPOT and request duration. Onyx has none of these. Adding `/metrics` with both llama.cpp-compatible names and OTel-aligned histograms is low-cost and high-value for JX Runtime dashboards.

### Cited Findings
- llama.cpp `GET /metrics` requires `--metrics`. Metrics: `llamacpp:prompt_tokens_total`, `prompt_seconds_total`, `prompt_tokens_seconds`, `tokens_predicted_total`, `tokens_predicted_seconds_total`, `predicted_tokens_seconds`, `requests_processing`, `requests_deferred`, `n_tokens_max`, `n_decode_total`, `n_busy_slots_per_decode`, plus `spec_decode_*` — [llama.cpp README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md); [server-task.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-task.cpp)
- vLLM metrics, all labeled `model_name`: `vllm:num_requests_running`, `vllm:num_requests_waiting`, `vllm:kv_cache_usage_perc`, and histograms `vllm:time_to_first_token_seconds`, `vllm:inter_token_latency_seconds`, `vllm:request_time_per_output_token_seconds` and `vllm:e2e_request_latency_seconds`.
- More vLLM metrics: counters `vllm:prompt_tokens_total`, `vllm:generation_tokens_total`, `vllm:request_success_total{finished_reason}`, `vllm:prefix_cache_queries` and `vllm:prefix_cache_hits`, plus `vllm:request_queue_time_seconds`, `vllm:request_prefill_time_seconds` and `vllm:request_decode_time_seconds`. The hit-rate gauge was replaced by the two counters — [vLLM metrics design](https://docs.vllm.ai/en/latest/design/metrics/)
- OTel GenAI semconv moved to the repo `open-telemetry/semantic-conventions-genai` — [opentelemetry.io gen-ai metrics page](https://opentelemetry.io/docs/specs/semconv/gen-ai/gen-ai-metrics/)
- OTel server metrics, all histograms in seconds at Development stability: `gen_ai.server.request.duration` (buckets 0.01–81.92 s), `gen_ai.server.time_to_first_token` (0.001–10 s; includes queueing and prefill) and `gen_ai.server.time_per_output_token` (0.01–2.5 s; decode phase).
- OTel attributes: required `gen_ai.operation.name` and `gen_ai.provider.name`; conditionally `gen_ai.request.model`, `error.type` and `server.port`; recommended `gen_ai.response.model` and `server.address`. Token-usage metrics are in a separate token-metrics document — [OTel semconv-genai metrics](https://github.com/open-telemetry/semantic-conventions-genai/blob/main/docs/gen-ai/gen-ai-metrics.md)

### Inferences
- `/metrics` for Onyx:
  - Format: Prometheus text format, optionally auth-exempt or behind a flag like llama.cpp.
  - llama.cpp-compatible names (`llamacpp:*`) so existing Grafana dashboards work.
  - Histograms: TTFT, TPOT/ITL, e2e latency and queue time.
  - Gauges: KV-cache usage, busy slots, waiting requests.
  - Counters: prompt and generation tokens, cache-hit tokens (Onyx already tracks `cache_n` in `timings`), and finished requests by `finish_reason`.
- Request IDs:
  - Echo or generate `X-Request-Id`, and use it as the `chatcmpl-` id suffix or log key.
  - Emit structured JSON access logs (id, route, status, prompt/completion tokens, TTFT, duration).
  - Optionally accept W3C `traceparent` so JX Runtime spans connect. This is my inference; I fetched no source on request-ID conventions specific to these servers.

### Gaps
- I did not fetch the OTel GenAI span conventions (`gen_ai.*` span names and attributes for inference) or the token-usage metric names.
- I found no primary source on `X-Request-Id` handling in llama.cpp or vLLM. The vLLM docs page I fetched did not document it.

## 7. Health/readiness and /v1/models metadata

### Takeaway
The convention set by llama.cpp is to start listening immediately and return `503 {"error":{"code":503,"message":"Loading model","type":"unavailable_error"}}` from `/health` while loading, then `200 {"status":"ok"}`. Onyx's "don't listen until loaded" model is simpler but gives no loading signal, so JX Runtime cannot distinguish loading from crashed except by timeout. For `/v1/models`, llama.cpp now publishes `meta.n_ctx` (per-slot), `n_ctx_train`, `n_vocab`, `vocab_type`, `ftype`, `aliases`, `tags` and `architecture.input_modalities`/`output_modalities`. Onyx has those values in `/props` but not in `/v1/models`.

### Cited Findings
- llama.cpp `/health` (alias `/v1/health`) is public and returns 503 while the model loads, then 200 `{"status":"ok"}`. The 503 body has `error.code` 503 and type `unavailable_error` — [llama.cpp README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md); [server-http.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-http.cpp)
- llama.cpp `/slots?fail_on_no_slot=1` returns 503 when no slot is free (usable as a saturation/readiness probe) — [llama.cpp README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
- llama.cpp `/v1/models` entry: `id`, `aliases`, `tags`, `object`, `architecture` (input/output modalities incl. image/audio/video), `created`, `owned_by:"llamacpp"`, and `meta` {`vocab_type`, `n_vocab`, `n_ctx` (slot ctx), `n_ctx_train`, `n_embd`, `n_params`, `size`, `ftype`}. `meta` can be `null` while loading — [server-context.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server-context.cpp); [README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
- llama.cpp router mode adds `POST /models/load`, `/models/unload`, `GET /models/sse` and `DELETE /models` — [server.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server.cpp)
- vLLM exposes `/health`, `/ping` (SageMaker), `/version`, `/load` and `/v1/models` — [vLLM online serving docs](https://docs.vllm.ai/en/latest/serving/online_serving/)
- Ollama `/v1/models` supports `/v1/models/{model}`. `created` is the model's last-modified time, and `owned_by` is the owner, defaulting to "library" — [Ollama OpenAI compatibility](https://docs.ollama.com/api/openai-compatibility)
- Continue lets users set `contextLength` manually when auto-detection is insufficient. Its example uses the `openai` provider with a local `/v1` base URL — [Continue configuration docs](https://docs.continue.dev/setup/configuration)

### Inferences
- Add `meta.n_ctx` (per-slot) to Onyx's `/v1/models`, plus `architecture.input_modalities` from `modalities`. Also consider `context_length` (OpenRouter-style) and `max_model_len` (vLLM-style) aliases, since clients probe different keys.
- Add `GET /v1/models/{id}`, which the OpenAI SDK `models.retrieve` uses. Use a stable `created` (model file mtime) instead of the current time, so clients that cache or sort models do not see churn.
- Optionally bind the socket early and serve 503 `unavailable_error` on all routes during load. This gives JX Runtime a loading state (`/health` 503 vs connection refused) and matches llama-server, which JX already knows. If implemented, update JX's `_waitReady()` semantics (docs/jx-runtime-integration.md).
- Add `/v1/health` as an alias of `/health`.

### Gaps
- I did not verify which `/v1/models` metadata keys Open WebUI, Cline and LibreChat actually read for context length. The search results did not cover Cline or LM Studio detection, and Continue's auto-detection mechanism is not documented in what I fetched.

## 8. MCP relevance

### Takeaway
MCP is a client/agent-side protocol. Inference servers relate to it only through tool calling, or through "remote MCP" tool types in Responses-style APIs. For Onyx, the relevant work is robust function calling plus pass-through of MCP-derived tools. Hosting MCP is out of scope.

### Cited Findings
- Open Responses classifies MCP servers as "externally hosted" tools alongside developer-run functions, distinct from provider-executed hosted tools — [Open Responses spec](https://www.openresponses.org/specification)
- LM Studio exposes MCP via its own API ("Using MCP via API"; connects tools and MCP servers locally). It is an app-level feature on top of its server — [LM Studio developer docs](https://lmstudio.ai/docs/developer)
- llama.cpp master has an optional `/tools` route (403 unless enabled) and a `/cors-proxy` route used by its web UI, not by the OpenAI API — [server.cpp](https://raw.githubusercontent.com/ggml-org/llama.cpp/master/tools/server/server.cpp)

### Inferences
- Onyx should not implement MCP. It should implement the `allowed_tools` constraint and correct multi-turn tool replay. If Responses is added, it should reject `type:"mcp"` tools with a clear `invalid_request` error, or leave them to JX Runtime to resolve.

### Gaps
- I did not review the OpenAI Responses `mcp` tool type schema or the MCP spec itself (out of scope for an inference server).
