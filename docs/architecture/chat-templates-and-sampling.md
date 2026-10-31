[Documentation](../README.md) › [Architecture](README.md) › Chat templates, grammar and sampling

# Chat templates, grammar and sampling

Templates, grammars from schemas and output parsing flow through llama.cpp's vendored `common` library; the sampler is Onyx's own, on `llama.h`. [The roadmap](../../ROADMAP.md#later) has the plan for owning more of it.

## The chat seam

Everything Onyx needs from llama.cpp's chat layer goes through `src/onyx_chat.h`
(Stage 1 of the [`common/` plan](../../reports/Replacing%20llama%20cpp%20common%20code.md)):

| Onyx type | What it does | What it wraps today |
|---|---|---|
| `onyx_chat_templates` | Loads the model's templates; `render(body, vocab)` turns an OpenAI chat body into an `onyx_chat_render` | `common_chat_templates_init`, `common_chat_templates_apply` and the `*_parse_oaicompat` helpers |
| `onyx_chat_render` | The rendered `onyx_prompt` (template and input parts), extra stops, the grammar in sampler terms (`apply_sampling()`), whether JSON output was asked for, the thinking tags, and `make_parser()` | `common_chat_params` |
| `onyx_chat_parser` | `message(text)` for a whole response; `update(text, is_partial, deltas)` for OpenAI stream deltas | `common_chat_parse`, `common_chat_msg_diff` |
| `onyx_jinja_template` | Renders a raw Jinja template with JSON globals (the decision models' `systemone` template) | `common_chat_template` and the Jinja runtime |

`onyx_chat.cpp` is the only file that includes `common/chat.h` or the Jinja headers.
A pin bump that reshapes `common_chat_*` (upstream's `common_chat_session` change, for one)
is absorbed there, and Stage 3 can replace the implementation behind the same
types family by family. `server.cpp`, `engine.cpp` and `decision.cpp` see only the
seam's types; `decision.cpp` still takes `common.h`'s string helpers.

## Chat templates

`onyx_engine::load` builds an `onyx_chat_templates`
with the model plus an optional override (`--chat-template` or the
contents of `--chat-template-file`). `jinja` is always used — the `--jinja`
flag exists only for command-line compatibility and is not itself a toggle
in this codebase.

## Applying a template

`onyx_chat_templates::render` reads `messages`, `tools`,
`tool_choice`, `parallel_tool_calls`, `grammar`, `json_schema`,
`response_format` and `chat_template_kwargs` from the request body and
applies the template. The result carries the final prompt
(`onyx_prompt`, split into template parts and request-input parts) plus any
grammar the template itself wants applied for tool calling.
`onyx_engine::tokenize_prompt()` parses special tokens in the template parts
only, so a message that spells out a control token such as `<|im_end|>` gets
plain text, not the token. With media the prompt goes to mtmd as one string
(`prompt.str()`), as before.

## Grammar / structured output

`onyx_chat_render::apply_sampling()` copies the grammar the template
produced (from `tools`/`tool_choice`, or from `json_schema`/`response_format`),
its triggers and preserved tokens into the request's `onyx_sampling_params`.
The grammar kind is chosen per request: `ONYX_GRAMMAR_USER` when the caller
passed `grammar` directly, `ONYX_GRAMMAR_OUTPUT_FORMAT` when
`json_schema`/`response_format` was used, otherwise `ONYX_GRAMMAR_TOOL_CALLS`
(letting the template's own tool-call grammar apply, if any). The Jinja path
builds no grammar from a caller's `grammar`, so when the template returns none
the caller's GBNF is used as given. A trigger word
that is a single preserved token becomes a token trigger. `/v1/completions`
builds its grammar more directly: a caller-supplied `grammar` string is used
as-is, or a `json_schema` is converted with `json_schema_to_grammar()`.

## Sampling

`parse_sampling()` reads `temperature`, `top_p`, `top_k`,
`min_p`, `seed`, `repeat_penalty`, `repeat_last_n`, `presence_penalty`, and
`frequency_penalty` from the request body into an `onyx_sampling_params`
(`src/sampler.h`), whose defaults are upstream's `common_params_sampling`
defaults. `onyx_engine::generate()` builds an `onyx_sampler` from it on the
HTTP thread, so a bad grammar or penalty fails the request before it is
queued; the engine thread then calls `sample()` and `accept()` once per token.

`onyx_sampler` is Onyx code on `llama.h`'s sampler API (Stage 1 of the
[`common/` plan](../../reports/Replacing%20llama%20cpp%20common%20code.md)),
replacing `common_sampler`. The chain is upstream's default order for the
parameters Onyx accepts: a logit bias for the vocabulary's suppress tokens
(when it has any), penalties, top-k, top-p, min-p, temperature, then `greedy`
at temperature 0 or `dist` with the request's seed otherwise. DRY,
top-n-sigma, typical-p and XTC are left out: Onyx refuses their request fields,
so they would only ever be identity samplers. The sampling algorithms
themselves stay llama.cpp's.

The grammar sampler sits outside the chain and is applied by rejection, as
upstream does: the chain picks a token, the grammar checks only that token,
and only if it refuses is the whole vocabulary masked and the chain run again.
All slots sample on the one engine thread, so masking the vocabulary on every
token would slow every slot. A lazy grammar's triggers become
`llama_sampler_init_grammar_lazy_patterns` arguments: words are regex-escaped,
full patterns anchored `^…$`, token triggers passed as ids. A schema or
tool-call grammar is first advanced past the template's generation prompt; the
caller's own `grammar` never is. A token forced by the reasoning budget goes to
the chain (penalties) but not the grammar. A grammar that leaves no token
allowed fails that request only, not the engine tick.

The exit test for the swap was token-for-token equality with `common_sampler`:
23 requests (greedy, seeded, penalties, `top_k` 1, negative temperature,
logprobs, user grammars, `json_schema`, `response_format`, lazy and required
tool-call grammars, streaming, and the error cases) gave identical responses
before and after on SmolLM2-135M-Instruct, Qwen2.5-0.5B-Instruct and the
smoke test's tiny model at `-np 2`.

## Parsing generated text back into a structured message

For chat completions, `onyx_chat_parser::message()` turns the raw generated
text into an OpenAI message (content, tool calls, reasoning content) with the
parser the template selected and the tokenized generation prompt; text it
cannot parse comes back as plain content. Streaming calls `update()` with the
text so far: it parses it as partial output, diffs it against the previous
parse and returns only the non-empty deltas, or `false` (hold the text) when
it cannot parse yet. The end of a stream calls it once more with the whole
text, not partial, to flush whatever was held.
