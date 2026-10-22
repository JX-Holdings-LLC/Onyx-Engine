[Documentation](../README.md) › [Architecture](README.md) › Chat templates, grammar and sampling

# Chat templates, grammar and sampling

Everything here flows through llama.cpp's vendored `common` library rather than being reimplemented. [The roadmap](../../ROADMAP.md#later) has the plan for owning more of it.

## Chat templates

`onyx_engine::load` calls `common_chat_templates_init`
with the model plus an optional override (`--chat-template` or the
contents of `--chat-template-file`), producing a
`common_chat_templates_ptr`. `jinja` is always used (`common_chat_templates_apply`
with `inputs.use_jinja = true`) — the `--jinja` flag exists only for
command-line compatibility and is not itself a toggle in this codebase.

## Applying a template

`server.cpp`'s `parse_chat_inputs` builds a
`common_chat_templates_inputs` from the request body (`messages`, `tools`,
`tool_choice`, `parallel_tool_calls`, `grammar`, `json_schema`,
`response_format`, `chat_template_kwargs`), and
`common_chat_templates_apply(engine.chat_templates(), inputs)` renders it
to a `common_chat_params` (`cp`), which carries the final prompt text
(`cp.prompt`, a `common_chat_prompt` split into template parts and
request-input parts) plus any grammar the template itself wants applied
for tool calling. `onyx_engine::tokenize_prompt()` parses special tokens
in the template parts only, so a message that spells out a control token
such as `<|im_end|>` gets plain text, not the token. With media the prompt
goes to mtmd as one string (`cp.prompt.str()`), as before.

## Grammar / structured output

`apply_grammar()` in `server.cpp` takes
whatever `common_chat_params` produced (from `tools`/`tool_choice`, from a
caller-supplied `grammar` string, or from `json_schema`/`response_format`)
and copies it — including grammar triggers and preserved tokens — into the
`common_params_sampling` that will be handed to the sampler. `grammar_type`
is chosen per request: `COMMON_GRAMMAR_TYPE_USER` when the caller passed
`grammar` directly, `COMMON_GRAMMAR_TYPE_OUTPUT_FORMAT` when
`json_schema`/`response_format` was used, otherwise
`COMMON_GRAMMAR_TYPE_TOOL_CALLS` (letting the template's own tool-call
grammar apply, if any). `/v1/completions` builds its grammar more directly:
a caller-supplied `grammar` string is used as-is, or a `json_schema` is
converted with `json_schema_to_grammar()`.

## Sampling

`parse_sampling()` reads `temperature`, `top_p`, `top_k`,
`min_p`, `seed`, `repeat_penalty`, `repeat_last_n`, `presence_penalty`, and
`frequency_penalty` from the request body into a `common_params_sampling`.
`onyx_engine::generate()` constructs a `common_sampler` from that struct via
`common_sampler_init` and drives it with `common_sampler_sample`/
`common_sampler_accept` — the sampling algorithms themselves (top-k/top-p/
min-p/repetition penalties/grammar-constrained sampling) are llama.cpp's,
not reimplemented here.

## Parsing generated text back into a structured message

For chat
completions, `common_chat_parse()` turns the raw generated text back into a
`common_chat_msg` (content, tool calls, reasoning content), using the
parser the template selected (`cp.parser`, copied into
`common_chat_parser_params` with the tokenized generation prompt). Streaming diffs
this against the previous partial parse (`common_chat_msg_diff::compute_diffs`)
to emit only the incremental delta each step.

