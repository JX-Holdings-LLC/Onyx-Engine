#include "onyx_chat.h"

#include "llama_util.h"

// the only file that includes llama.cpp's chat layer
#include "chat.h"
#include "common.h"
#include "jinja/runtime.h"
#include "jinja/value.h"
#include "json-schema-to-grammar.h"

#include <stdexcept>

std::string onyx_prompt::str() const {
    std::string out;
    for (const auto & part : parts) {
        out += part.text;
    }
    return out;
}

// Onyx's JSON and llama.cpp's common_json cross as text: common_json keeps its
// nlohmann value private, and a dump/parse costs little next to rendering.
static common_json to_common(const onyx_json & j) {
    return common_json::parse(j.dump());
}

static onyx_json from_common(const common_json & j) {
    return onyx_json::parse(j.dump());
}

std::string onyx_json_schema_to_grammar(const onyx_json & schema) {
    return json_schema_to_grammar(to_common(schema));
}

// ---------------------------------------------------------------------------
// request -> template inputs
// ---------------------------------------------------------------------------

static common_chat_templates_inputs parse_chat_inputs(const onyx_json & body) {
    common_chat_templates_inputs inputs;
    inputs.messages         = common_chat_msgs_parse_oaicompat(to_common(body.at("messages")));
    inputs.use_jinja        = true;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;

    if (body.contains("tools") && body.at("tools").is_array() && body.at("tools").size() > 0) {
        inputs.tools = common_chat_tools_parse_oaicompat(to_common(body.at("tools")));
    }
    if (body.contains("tool_choice") && body.at("tool_choice").is_string()) {
        inputs.tool_choice = common_chat_tool_choice_parse_oaicompat(body.at("tool_choice").get<std::string>());
    } else if (body.contains("tool_choice") && body.at("tool_choice").is_object()) {
        const auto & choice = body.at("tool_choice");
        if (choice.value<std::string>("type", "") != "function" || !choice.contains("function") ||
            !choice.at("function").is_object() || !choice.at("function").contains("name")) {
            throw std::invalid_argument("tool_choice must name a function");
        }
        const std::string name = choice.at("function").at("name").get<std::string>();
        std::vector<common_chat_tool> selected;
        for (const auto & tool : inputs.tools) {
            if (tool.name == name) selected.push_back(tool);
        }
        if (selected.empty()) throw std::invalid_argument("tool_choice names an unknown function: " + name);
        inputs.tools = std::move(selected);
        inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    } else if (body.contains("tool_choice") && !body.at("tool_choice").is_null()) {
        throw std::invalid_argument("tool_choice must be a string or function object");
    }
    if (body.contains("parallel_tool_calls")) {
        inputs.parallel_tool_calls = body.at("parallel_tool_calls").get<bool>();
    }
    if (body.contains("grammar") && body.at("grammar").is_string()) {
        inputs.grammar = body.at("grammar").get<std::string>();
    }
    if (body.contains("json_schema")) {
        inputs.json_schema = body.at("json_schema").dump();
    }
    if (body.contains("response_format") && body.at("response_format").is_object()) {
        const onyx_json & rf = body.at("response_format");
        const std::string type = rf.value<std::string>("type", "");
        if (type == "json_object") {
            // OpenAI's JSON mode promises an object; "{}" would leave the
            // output unconstrained, since common/ builds no grammar for it
            inputs.json_schema = "{\"type\":\"object\"}";
            if (rf.contains("schema")) {
                inputs.json_schema = rf.at("schema").dump();
            }
        } else if (type == "json_schema") {
            if (rf.contains("json_schema") && rf.at("json_schema").contains("schema")) {
                inputs.json_schema = rf.at("json_schema").at("schema").dump();
            } else if (rf.contains("schema")) {
                inputs.json_schema = rf.at("schema").dump();
            }
        } else if (!type.empty() && type != "text") {
            throw std::runtime_error("unsupported response_format type: " + type);
        }
    }
    if (body.contains("chat_template_kwargs") && body.at("chat_template_kwargs").is_object()) {
        for (const auto & [key, val] : body.at("chat_template_kwargs").items()) {
            inputs.chat_template_kwargs[key] = val.is_string() ? val.get<std::string>() : val.dump();
        }
    }
    return inputs;
}

// ---------------------------------------------------------------------------
// templates
// ---------------------------------------------------------------------------

struct onyx_chat_templates::impl {
    common_chat_templates_ptr tmpls;
};

onyx_chat_templates::onyx_chat_templates(const llama_model * model, const std::string & override_src)
    : impl_(std::make_unique<impl>()) {
    impl_->tmpls = common_chat_templates_init(model, override_src);
}

onyx_chat_templates::~onyx_chat_templates() = default;

std::string onyx_chat_templates::source() const {
    return common_chat_templates_source(impl_->tmpls.get());
}

struct onyx_chat_render::parser_seed {
    common_chat_parser_params params;
};

onyx_chat_render onyx_chat_templates::render(const onyx_json & body, const llama_vocab * vocab) const {
    const common_chat_templates_inputs inputs = parse_chat_inputs(body);
    const common_chat_params           cp     = common_chat_templates_apply(impl_->tmpls.get(), inputs);

    onyx_chat_render out;
    for (const auto & part : cp.prompt.parts) {
        out.prompt.parts.push_back({ part.text, part.is_input });
    }
    out.additional_stops = cp.additional_stops;
    out.output_format    = inputs.grammar.empty() && !inputs.json_schema.empty();
    if (cp.supports_thinking) {
        out.thinking_start_tag = cp.thinking_start_tag;
        out.thinking_end_tag   = cp.thinking_end_tags.empty() ? std::string() : cp.thinking_end_tags[0];
    }

    // control tokens the template wants rendered as text when generated
    for (const auto & t : cp.preserved_tokens) {
        const auto ids = onyx_tokenize(vocab, t, /* add_special */ false, /* parse_special */ true);
        if (ids.size() == 1) {
            out.preserved_tokens.insert(ids[0]);
        }
    }

    if (!cp.grammar.empty()) {
        out.grammar_kind      = !inputs.grammar.empty()     ? ONYX_GRAMMAR_USER :
                                !inputs.json_schema.empty() ? ONYX_GRAMMAR_OUTPUT_FORMAT
                                                            : ONYX_GRAMMAR_TOOL_CALLS;
        out.grammar           = cp.grammar;
        out.grammar_lazy      = cp.grammar_lazy;
        out.generation_prompt = cp.generation_prompt;

        for (const auto & trigger : cp.grammar_triggers) {
            onyx_grammar_trigger t;
            t.value = trigger.value;
            switch (trigger.type) {
                case COMMON_GRAMMAR_TRIGGER_TYPE_WORD:         t.kind = ONYX_TRIGGER_WORD;         break;
                case COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN:      t.kind = ONYX_TRIGGER_PATTERN;      break;
                case COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL: t.kind = ONYX_TRIGGER_PATTERN_FULL; break;
                case COMMON_GRAMMAR_TRIGGER_TYPE_TOKEN:
                    t.kind  = ONYX_TRIGGER_TOKEN;
                    t.token = trigger.token;
                    break;
            }
            if (t.kind == ONYX_TRIGGER_WORD) {
                // a word that is one preserved token triggers on that token
                const auto ids = onyx_tokenize(vocab, trigger.value, false, true);
                if (ids.size() == 1 && out.preserved_tokens.count(ids[0]) > 0) {
                    t.kind  = ONYX_TRIGGER_TOKEN;
                    t.token = ids[0];
                }
            }
            out.grammar_triggers.push_back(std::move(t));
        }
    } else if (!inputs.grammar.empty()) {
        // the Jinja path builds no grammar from the caller's own GBNF
        // (llama-server applies it from the request, as here)
        out.grammar_kind = ONYX_GRAMMAR_USER;
        out.grammar      = inputs.grammar;
    }

    auto seed = std::make_shared<onyx_chat_render::parser_seed>();
    seed->params                   = common_chat_parser_params(cp);
    seed->params.generation_prompt = common_chat_input_tokenize(vocab, cp.generation_prompt);
    seed->params.parser            = cp.parser;
    out.seed = std::move(seed);
    return out;
}

void onyx_chat_render::apply_sampling(onyx_sampling_params & sampling) const {
    sampling.preserved_tokens.insert(preserved_tokens.begin(), preserved_tokens.end());
    if (grammar.empty()) {
        return;
    }
    sampling.grammar_kind      = grammar_kind;
    sampling.grammar           = grammar;
    sampling.grammar_lazy      = grammar_lazy;
    sampling.generation_prompt = generation_prompt;
    sampling.grammar_triggers.insert(sampling.grammar_triggers.end(), grammar_triggers.begin(), grammar_triggers.end());
}

std::unique_ptr<onyx_chat_parser> onyx_chat_render::make_parser() const {
    return std::make_unique<onyx_chat_parser>(seed);
}

// ---------------------------------------------------------------------------
// output parsing
// ---------------------------------------------------------------------------

struct onyx_chat_parser::impl {
    std::shared_ptr<const onyx_chat_render::parser_seed> seed;
    common_chat_msg                                      prev;
};

onyx_chat_parser::onyx_chat_parser(std::shared_ptr<const onyx_chat_render::parser_seed> seed)
    : impl_(std::make_unique<impl>()) {
    impl_->seed = std::move(seed);
}

onyx_chat_parser::~onyx_chat_parser() = default;

// OpenAI delta object for one parsed-message diff (mirrors llama-server)
static onyx_json diff_to_delta(const common_chat_msg_diff & diff) {
    onyx_json delta = onyx_json::object();
    if (!diff.reasoning_content_delta.empty()) {
        delta["reasoning_content"] = diff.reasoning_content_delta;
    }
    if (!diff.content_delta.empty()) {
        delta["content"] = diff.content_delta;
    }
    if (diff.tool_call_index != std::string::npos) {
        onyx_json tc = onyx_json::object();
        tc["index"] = (int64_t) diff.tool_call_index;
        if (!diff.tool_call_delta.id.empty()) {
            tc["id"]   = diff.tool_call_delta.id;
            tc["type"] = "function";
        }
        if (!diff.tool_call_delta.name.empty() || !diff.tool_call_delta.arguments.empty()) {
            onyx_json fn = onyx_json::object();
            if (!diff.tool_call_delta.name.empty()) {
                fn["name"] = diff.tool_call_delta.name;
            }
            if (!diff.tool_call_delta.arguments.empty()) {
                fn["arguments"] = diff.tool_call_delta.arguments;
            }
            tc["function"] = fn;
        }
        onyx_json arr = onyx_json::array();
        arr.push_back(tc);
        delta["tool_calls"] = arr;
    }
    return delta;
}

onyx_json onyx_chat_parser::message(const std::string & text) {
    common_chat_msg msg;
    try {
        msg = common_chat_parse(common_chat_input(text), /* is_partial */ false, impl_->seed->params);
    } catch (const std::exception &) {
        msg         = {};
        msg.content = text;
    }
    if (msg.role.empty()) {
        msg.role = "assistant";
    }
    impl_->prev = msg;
    return from_common(msg.to_json_oaicompat(/* concat_typed_text */ true));
}

bool onyx_chat_parser::update(const std::string & text, bool is_partial, std::vector<onyx_json> & deltas) {
    std::vector<common_chat_msg_diff> diffs;
    try {
        common_chat_msg cur = common_chat_parse(common_chat_input(text), is_partial, impl_->seed->params);
        if (cur.role.empty()) {
            cur.role = "assistant";
        }
        diffs = common_chat_msg_diff::compute_diffs(impl_->prev, cur);
        impl_->prev = std::move(cur);
    } catch (const std::exception &) {
        return false;
    }
    for (const auto & diff : diffs) {
        onyx_json delta = diff_to_delta(diff);
        if (delta.size() > 0) {
            deltas.push_back(std::move(delta));
        }
    }
    return true;
}

bool onyx_chat_parser::has_tool_calls() const {
    return !impl_->prev.tool_calls.empty();
}

// ---------------------------------------------------------------------------
// raw Jinja
// ---------------------------------------------------------------------------

struct onyx_jinja_template::impl {
    common_chat_template tmpl;
    explicit impl(const std::string & source) : tmpl(source, "", "") {}
};

onyx_jinja_template::onyx_jinja_template(const std::string & source)
    : impl_(std::make_unique<impl>(source)) {}

onyx_jinja_template::~onyx_jinja_template() = default;

std::string onyx_jinja_template::render(const onyx_json & globals) const {
    jinja::context ctx(impl_->tmpl.source());
    jinja::global_from_json(ctx, to_common(globals), /* mark_input */ false);
    jinja::runtime runtime(ctx);
    const jinja::value results = runtime.execute(impl_->tmpl.prog);
    return jinja::runtime::gather_string_parts(results)->as_string().str();
}
