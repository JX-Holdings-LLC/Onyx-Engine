// The chat seam: onyx-engine's own interface in front of llama.cpp's chat
// layer (common/chat.h and its Jinja engine).
//
// Stage 1 of the common/ plan (reports/Replacing llama cpp common code.md).
// Everything the server and the decision code need from the chat layer goes
// through these types: rendering a chat request with the model's template,
// the grammar and stops that come with it, parsing generated text back into
// an OpenAI message (whole or as streamed deltas), and rendering a raw Jinja
// template. Only onyx_chat.cpp includes common/chat.h, so a pin bump that
// reshapes common_chat_* (or a later Onyx-owned chat layer, Stage 3) changes
// that one file.
#pragma once

#include "onyx_json.h"
#include "sampler.h"

#include "llama.h"

#include <memory>
#include <string>
#include <vector>

// A rendered prompt, split into template text and text that came from the
// request. Special tokens are parsed in template text only, so a message that
// spells out "<|im_end|>" stays text.
struct onyx_prompt_part {
    std::string text;
    bool        is_input = false;
};

struct onyx_prompt {
    std::vector<onyx_prompt_part> parts;

    std::string str() const;
};

class onyx_chat_parser;

// What the template produced for one chat request.
class onyx_chat_render {
public:
    onyx_prompt              prompt;
    std::vector<std::string> additional_stops;  // stops the template asks for

    // The grammar the request runs under, in sampler terms: the template's
    // tool-call or schema grammar, or the caller's own GBNF.
    onyx_grammar_kind                 grammar_kind = ONYX_GRAMMAR_NONE;
    std::string                       grammar;
    bool                              grammar_lazy = false;
    std::vector<onyx_grammar_trigger> grammar_triggers;
    std::set<llama_token>             preserved_tokens;
    std::string                       generation_prompt;

    // the request asked for JSON (json_schema or response_format)
    bool output_format = false;

    // the template's own thinking tags, empty when it has none
    std::string thinking_start_tag;
    std::string thinking_end_tag;

    // Copies the grammar fields above into `sampling`.
    void apply_sampling(onyx_sampling_params & sampling) const;

    // A parser for this request's output (one per response).
    std::unique_ptr<onyx_chat_parser> make_parser() const;

    struct parser_seed;
    std::shared_ptr<const parser_seed> seed;   // opaque, for make_parser()
};

// Turns generated text back into an OpenAI assistant message.
class onyx_chat_parser {
public:
    explicit onyx_chat_parser(std::shared_ptr<const onyx_chat_render::parser_seed> seed);
    ~onyx_chat_parser();

    // The whole output as an OpenAI message object ({"role", "content",
    // "tool_calls", "reasoning_content"}). Text the parser cannot read is
    // returned as plain content.
    onyx_json message(const std::string & text);

    // Parses the output so far and appends to `deltas` the OpenAI delta
    // objects for what changed since the last call (empty ones are not
    // added). Returns false, with nothing appended, when the text cannot be
    // parsed yet.
    bool update(const std::string & text, bool is_partial, std::vector<onyx_json> & deltas);

    // the last parsed message carries tool calls
    bool has_tool_calls() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// The model's chat templates.
class onyx_chat_templates {
public:
    // `override_src` is Jinja source, "chatml" or empty (the model's own).
    // Throws std::exception when the template cannot be loaded.
    onyx_chat_templates(const llama_model * model, const std::string & override_src);
    ~onyx_chat_templates();

    std::string source() const;

    // Renders an OpenAI chat request body (messages, tools, tool_choice,
    // parallel_tool_calls, grammar, json_schema, response_format,
    // chat_template_kwargs). Media parts must already be media_marker parts.
    // Throws std::invalid_argument or std::runtime_error on a bad request.
    onyx_chat_render render(const onyx_json & body, const llama_vocab * vocab) const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// A raw Jinja template rendered with JSON globals (the decision models'
// "systemone" template).
class onyx_jinja_template {
public:
    explicit onyx_jinja_template(const std::string & source);   // throws on a parse error
    ~onyx_jinja_template();

    std::string render(const onyx_json & globals) const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};
