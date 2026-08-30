#include "server.h"

#include "build-info.h"
#include "chat.h"
#include "common.h"
#include "json-schema-to-grammar.h"
#include "json.h"
#include "log.h"
#include "sampling.h"

#include "httplib.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static std::string gen_request_id(const char * prefix) {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%016" PRIx64, prefix, (uint64_t) rng());
    return buf;
}

static void send_error(httplib::Response & res, int status, const std::string & message, const char * type) {
    common_json err = common_json::object();
    common_json body = common_json::object();
    err["message"] = message;
    err["type"]    = type;
    err["code"]    = status;
    body["error"]  = err;
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

static std::string finish_reason_str(const jx_gen_result & r, bool has_tool_calls) {
    if (r.finish == JX_FINISH_LENGTH) {
        return "length";
    }
    return has_tool_calls ? "tool_calls" : "stop";
}

static common_json usage_json(const jx_gen_result & r) {
    common_json u = common_json::object();
    u["prompt_tokens"]     = r.n_prompt;
    u["completion_tokens"] = r.n_predicted;
    u["total_tokens"]      = r.n_prompt + r.n_predicted;
    return u;
}

static common_json timings_json(const jx_gen_result & r) {
    common_json t = common_json::object();
    t["cache_n"]              = r.n_cached;
    t["prompt_n"]             = r.n_prompt - r.n_cached;
    t["prompt_ms"]            = r.t_prompt_ms;
    t["prompt_per_second"]    = r.t_prompt_ms > 0 ? (r.n_prompt - r.n_cached) / r.t_prompt_ms * 1000.0 : 0.0;
    t["predicted_n"]          = r.n_predicted;
    t["predicted_ms"]         = r.t_predict_ms;
    t["predicted_per_second"] = r.t_predict_ms > 0 ? r.n_predicted / r.t_predict_ms * 1000.0 : 0.0;
    return t;
}

// OpenAI delta object for one parsed-message diff (mirrors llama-server)
static common_json diff_to_delta(const common_chat_msg_diff & diff) {
    common_json delta = common_json::object();
    if (!diff.reasoning_content_delta.empty()) {
        delta["reasoning_content"] = diff.reasoning_content_delta;
    }
    if (!diff.content_delta.empty()) {
        delta["content"] = diff.content_delta;
    }
    if (diff.tool_call_index != std::string::npos) {
        common_json tc = common_json::object();
        tc["index"] = (int64_t) diff.tool_call_index;
        if (!diff.tool_call_delta.id.empty()) {
            tc["id"]   = diff.tool_call_delta.id;
            tc["type"] = "function";
        }
        if (!diff.tool_call_delta.name.empty() || !diff.tool_call_delta.arguments.empty()) {
            common_json fn = common_json::object();
            if (!diff.tool_call_delta.name.empty()) {
                fn["name"] = diff.tool_call_delta.name;
            }
            if (!diff.tool_call_delta.arguments.empty()) {
                fn["arguments"] = diff.tool_call_delta.arguments;
            }
            tc["function"] = fn;
        }
        common_json arr = common_json::array();
        arr.push_back(tc);
        delta["tool_calls"] = arr;
    }
    return delta;
}

// ---------------------------------------------------------------------------
// request parsing
// ---------------------------------------------------------------------------

static std::vector<std::string> parse_stop(const common_json & body) {
    std::vector<std::string> stop;
    if (!body.contains("stop")) {
        return stop;
    }
    const common_json & s = body.at("stop");
    if (s.is_string()) {
        std::string v = s.get<std::string>();
        if (!v.empty()) {
            stop.push_back(std::move(v));
        }
    } else if (s.is_array()) {
        for (size_t i = 0; i < s.size(); i++) {
            std::string v = s[i].get<std::string>();
            if (!v.empty()) {
                stop.push_back(std::move(v));
            }
        }
    }
    return stop;
}

static int32_t parse_max_tokens(const common_json & body) {
    for (const char * key : {"max_completion_tokens", "max_tokens", "n_predict"}) {
        if (body.contains(key) && !body.at(key).is_null()) {
            return body.at(key).get<int32_t>();
        }
    }
    return -1;
}

static common_params_sampling parse_sampling(const common_json & body, uint32_t default_seed) {
    common_params_sampling s;
    s.seed = default_seed;

    if (body.contains("temperature") && !body.at("temperature").is_null()) s.temp = body.at("temperature").get<float>();
    if (body.contains("top_p"))             s.top_p           = body.at("top_p").get<float>();
    if (body.contains("top_k"))             s.top_k           = body.at("top_k").get<int32_t>();
    if (body.contains("min_p"))             s.min_p           = body.at("min_p").get<float>();
    if (body.contains("seed") && !body.at("seed").is_null()) s.seed = (uint32_t) body.at("seed").get<int64_t>();
    if (body.contains("repeat_penalty"))    s.penalty_repeat  = body.at("repeat_penalty").get<float>();
    if (body.contains("repeat_last_n"))     s.penalty_last_n  = body.at("repeat_last_n").get<int32_t>();
    if (body.contains("presence_penalty") && !body.at("presence_penalty").is_null())
        s.penalty_present = body.at("presence_penalty").get<float>();
    if (body.contains("frequency_penalty") && !body.at("frequency_penalty").is_null())
        s.penalty_freq = body.at("frequency_penalty").get<float>();

    return s;
}

// applies grammar produced by the chat template (tool calls / json_schema) or
// supplied directly by the caller
static void apply_grammar(common_params_sampling &   sparams,
                          const common_chat_params & cp,
                          common_grammar_type        type,
                          const llama_vocab *        vocab) {
    if (cp.grammar.empty()) {
        return;
    }
    sparams.grammar           = common_grammar(type, cp.grammar);
    sparams.grammar_lazy      = cp.grammar_lazy;
    sparams.generation_prompt = cp.generation_prompt;

    for (const auto & t : cp.preserved_tokens) {
        auto ids = common_tokenize(vocab, t, /* add_special */ false, /* parse_special */ true);
        if (ids.size() == 1) {
            sparams.preserved_tokens.insert(ids[0]);
        }
    }
    for (const auto & trigger : cp.grammar_triggers) {
        if (trigger.type == COMMON_GRAMMAR_TRIGGER_TYPE_WORD) {
            auto ids = common_tokenize(vocab, trigger.value, false, true);
            if (ids.size() == 1 && sparams.preserved_tokens.count(ids[0]) > 0) {
                common_grammar_trigger tok_trigger;
                tok_trigger.type  = COMMON_GRAMMAR_TRIGGER_TYPE_TOKEN;
                tok_trigger.value = trigger.value;
                tok_trigger.token = ids[0];
                sparams.grammar_triggers.push_back(std::move(tok_trigger));
                continue;
            }
        }
        sparams.grammar_triggers.push_back(trigger);
    }
}

// builds template inputs shared by /v1/chat/completions and /apply-template
static common_chat_templates_inputs parse_chat_inputs(const common_json & body) {
    common_chat_templates_inputs inputs;
    inputs.messages         = common_chat_msgs_parse_oaicompat(body.at("messages"));
    inputs.use_jinja        = true;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;

    if (body.contains("tools") && body.at("tools").is_array() && body.at("tools").size() > 0) {
        inputs.tools = common_chat_tools_parse_oaicompat(body.at("tools"));
    }
    if (body.contains("tool_choice") && body.at("tool_choice").is_string()) {
        inputs.tool_choice = common_chat_tool_choice_parse_oaicompat(body.at("tool_choice").get<std::string>());
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
        const common_json & rf = body.at("response_format");
        const std::string type = rf.value<std::string>("type", "");
        if (type == "json_object") {
            inputs.json_schema = "{}";
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
// chunk / response builders (chat)
// ---------------------------------------------------------------------------

struct chat_stream_state {
    std::string               request_id;
    std::string               model;
    std::time_t               created = 0;
    common_chat_parser_params parser_params;
    std::string               accumulated;
    common_chat_msg           prev_msg;
};

static common_json make_chat_chunk(const chat_stream_state & st, common_json delta, const char * finish_reason) {
    common_json choice = common_json::object();
    choice["index"] = 0;
    choice["delta"] = delta;
    if (finish_reason) {
        choice["finish_reason"] = finish_reason;
    } else {
        choice["finish_reason"] = nullptr;
    }
    common_json choices = common_json::array();
    choices.push_back(choice);

    common_json chunk = common_json::object();
    chunk["id"]      = st.request_id;
    chunk["object"]  = "chat.completion.chunk";
    chunk["created"] = (int64_t) st.created;
    chunk["model"]   = st.model;
    chunk["choices"] = choices;
    return chunk;
}

static bool sse_write(httplib::DataSink & sink, const common_json & payload) {
    const std::string frame = "data: " + payload.dump_safe() + "\n\n";
    return sink.write(frame.data(), frame.size());
}

// ---------------------------------------------------------------------------
// server
// ---------------------------------------------------------------------------

int jx_server_run(jx_engine & engine, const jx_args & args) {
    httplib::Server svr;

    svr.set_default_headers({
        {"Server", "jx-engine/" JX_ENGINE_VERSION},
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Authorization, Content-Type"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    });

    svr.Options(".*", [](const httplib::Request &, httplib::Response & res) {
        res.status = 204;
    });

    // bearer auth on everything except /health
    if (!args.api_key.empty()) {
        const std::string expected = "Bearer " + args.api_key;
        svr.set_pre_routing_handler([expected](const httplib::Request & req, httplib::Response & res) {
            if (req.path == "/health" || req.method == "OPTIONS") {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            if (req.get_header_value("Authorization") != expected) {
                send_error(res, 401, "invalid or missing API key", "authentication_error");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
    }

    svr.set_exception_handler([](const httplib::Request &, httplib::Response & res, std::exception_ptr ep) {
        std::string msg = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception & e) {
            msg = e.what();
        } catch (...) {
        }
        send_error(res, 500, msg, "server_error");
    });

    // ---- health / diagnostics -------------------------------------------

    svr.Get("/health", [](const httplib::Request &, httplib::Response & res) {
        res.set_content("{\"status\":\"ok\"}", "application/json; charset=utf-8");
    });

    svr.Get("/props", [&](const httplib::Request &, httplib::Response & res) {
        common_json defaults = common_json::object();
        defaults["n_ctx"] = (int64_t) engine.n_ctx();

        common_json modalities = common_json::object();
        modalities["vision"] = false;
        modalities["audio"]  = false;

        common_json props = common_json::object();
        props["model_alias"]                 = engine.alias();
        props["chat_template"]               = engine.chat_template_source();
        props["build_info"]                  = std::string("jx-engine/" JX_ENGINE_VERSION " (") + llama_build_info() + ")";
        props["n_ctx"]                       = (int64_t) engine.n_ctx();
        props["n_ctx_train"]                 = (int64_t) engine.n_ctx_train();
        props["n_embd"]                      = (int64_t) engine.n_embd();
        props["embedding_mode"]              = engine.embedding_mode();
        props["model_desc"]                  = engine.model_desc();
        props["model_size_bytes"]            = engine.model_size_bytes();
        props["model_n_params"]              = engine.model_n_params();
        props["modalities"]                  = modalities;
        props["default_generation_settings"] = defaults;
        res.set_content(props.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/v1/models", [&](const httplib::Request &, httplib::Response & res) {
        common_json meta = common_json::object();
        meta["n_ctx_train"] = (int64_t) engine.n_ctx_train();
        meta["n_embd"]      = (int64_t) engine.n_embd();
        meta["size"]        = engine.model_size_bytes();
        meta["n_params"]    = engine.model_n_params();

        common_json model = common_json::object();
        model["id"]       = engine.alias();
        model["object"]   = "model";
        model["created"]  = (int64_t) std::time(nullptr);
        model["owned_by"] = "jx-engine";
        model["meta"]     = meta;

        common_json data = common_json::array();
        data.push_back(model);

        common_json out = common_json::object();
        out["object"] = "list";
        out["data"]   = data;
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // ---- tokenization ---------------------------------------------------

    svr.Post("/tokenize", [&](const httplib::Request & req, httplib::Response & res) {
        const common_json body = common_json::parse(req.body);
        const std::string content = body.value<std::string>("content", "");
        const bool add_special    = body.value<bool>("add_special", false);

        const auto tokens = engine.tokenize(content, add_special, true);
        common_json toks = common_json::array();
        for (llama_token t : tokens) {
            toks.push_back((int64_t) t);
        }
        common_json out = common_json::object();
        out["tokens"] = toks;
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/detokenize", [&](const httplib::Request & req, httplib::Response & res) {
        const common_json body = common_json::parse(req.body);
        std::vector<llama_token> tokens;
        if (body.contains("tokens") && body.at("tokens").is_array()) {
            const common_json & arr = body.at("tokens");
            for (size_t i = 0; i < arr.size(); i++) {
                tokens.push_back(arr[i].get<int32_t>());
            }
        }
        common_json out = common_json::object();
        out["content"] = engine.detokenize(tokens, false);
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/apply-template", [&](const httplib::Request & req, httplib::Response & res) {
        const common_json body = common_json::parse(req.body);
        if (!body.contains("messages")) {
            send_error(res, 400, "missing 'messages'", "invalid_request_error");
            return;
        }
        const auto inputs = parse_chat_inputs(body);
        const auto cp     = common_chat_templates_apply(engine.chat_templates(), inputs);
        common_json out = common_json::object();
        out["prompt"] = cp.prompt;
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // ---- chat completions ------------------------------------------------

    svr.Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
        const common_json body = common_json::parse(req.body);
        if (!body.contains("messages") || !body.at("messages").is_array()) {
            send_error(res, 400, "missing or invalid 'messages'", "invalid_request_error");
            return;
        }
        if (engine.embedding_mode()) {
            send_error(res, 501, "this instance serves embeddings only (started with --embedding)", "not_supported_error");
            return;
        }

        const bool stream = body.value<bool>("stream", false);

        common_chat_templates_inputs inputs;
        common_chat_params           cp;
        try {
            inputs = parse_chat_inputs(body);
            cp     = common_chat_templates_apply(engine.chat_templates(), inputs);
        } catch (const std::exception & e) {
            send_error(res, 400, std::string("failed to apply chat template: ") + e.what(), "invalid_request_error");
            return;
        }

        jx_gen_params gp;
        gp.prompt_tokens = engine.tokenize(cp.prompt, /* add_special */ true, /* parse_special */ true);
        gp.n_predict     = parse_max_tokens(body);
        gp.stop          = parse_stop(body);
        for (const auto & s : cp.additional_stops) {
            gp.stop.push_back(s);
        }
        gp.sampling = parse_sampling(body, args.seed);

        const common_grammar_type gtype =
            !inputs.grammar.empty()     ? COMMON_GRAMMAR_TYPE_USER :
            !inputs.json_schema.empty() ? COMMON_GRAMMAR_TYPE_OUTPUT_FORMAT
                                        : COMMON_GRAMMAR_TYPE_TOOL_CALLS;
        apply_grammar(gp.sampling, cp, gtype, engine.vocab());

        common_chat_parser_params parser_params(cp);
        parser_params.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
        if (!cp.parser.empty()) {
            parser_params.parser.load(cp.parser);
        }

        const std::string request_id = gen_request_id("chatcmpl");
        const std::time_t created    = std::time(nullptr);

        if (!stream) {
            jx_gen_result r = engine.generate(gp, nullptr);
            if (!r.error.empty()) {
                send_error(res, 500, r.error, "server_error");
                return;
            }

            common_chat_msg msg;
            try {
                msg = common_chat_parse(r.text, /* is_partial */ false, parser_params);
            } catch (const std::exception & e) {
                msg = {};
                msg.role    = "assistant";
                msg.content = r.text;
            }
            if (msg.role.empty()) {
                msg.role = "assistant";
            }

            common_json message = msg.to_json_oaicompat(/* concat_typed_text */ true);
            common_json choice  = common_json::object();
            choice["index"]         = 0;
            choice["message"]       = message;
            choice["finish_reason"] = finish_reason_str(r, !msg.tool_calls.empty());
            common_json choices = common_json::array();
            choices.push_back(choice);

            common_json out = common_json::object();
            out["id"]      = request_id;
            out["object"]  = "chat.completion";
            out["created"] = (int64_t) created;
            out["model"]   = engine.alias();
            out["choices"] = choices;
            out["usage"]   = usage_json(r);
            out["timings"] = timings_json(r);
            res.set_content(out.dump_safe(), "application/json; charset=utf-8");
            return;
        }

        // streaming
        auto st = std::make_shared<chat_stream_state>();
        st->request_id    = request_id;
        st->model         = engine.alias();
        st->created       = created;
        st->parser_params = parser_params;

        res.set_chunked_content_provider("text/event-stream", [&engine, gp, st](size_t offset, httplib::DataSink & sink) {
            if (offset > 0) {
                // single-shot provider: everything is written on the first call
                return false;
            }

            {
                common_json role_delta = common_json::object();
                role_delta["role"]    = "assistant";
                role_delta["content"] = "";
                if (!sse_write(sink, make_chat_chunk(*st, role_delta, nullptr))) {
                    return false;
                }
            }

            auto emit_piece = [&](const std::string & piece) -> bool {
                st->accumulated += piece;
                std::vector<common_chat_msg_diff> diffs;
                try {
                    common_chat_msg cur = common_chat_parse(st->accumulated, /* is_partial */ true, st->parser_params);
                    if (cur.role.empty()) {
                        cur.role = "assistant";
                    }
                    diffs = common_chat_msg_diff::compute_diffs(st->prev_msg, cur);
                    st->prev_msg = std::move(cur);
                } catch (const std::exception &) {
                    // partial output not yet parseable - hold until more arrives
                    return sink.is_writable();
                }
                for (const auto & diff : diffs) {
                    common_json delta = diff_to_delta(diff);
                    if (delta.size() == 0) {
                        continue;
                    }
                    if (!sse_write(sink, make_chat_chunk(*st, delta, nullptr))) {
                        return false;
                    }
                }
                return true;
            };

            jx_gen_result r = engine.generate(gp, emit_piece);

            if (!r.error.empty()) {
                common_json err = common_json::object();
                common_json e   = common_json::object();
                e["message"] = r.error;
                e["type"]    = "server_error";
                err["error"] = e;
                sse_write(sink, err);
                sink.done();
                return true;
            }
            if (r.finish == JX_FINISH_CANCEL) {
                return false;
            }

            // flush any tail withheld by the partial parser
            try {
                common_chat_msg final_msg = common_chat_parse(r.text, /* is_partial */ false, st->parser_params);
                if (final_msg.role.empty()) {
                    final_msg.role = "assistant";
                }
                const auto diffs = common_chat_msg_diff::compute_diffs(st->prev_msg, final_msg);
                for (const auto & diff : diffs) {
                    common_json delta = diff_to_delta(diff);
                    if (delta.size() > 0) {
                        sse_write(sink, make_chat_chunk(*st, delta, nullptr));
                    }
                }
                st->prev_msg = std::move(final_msg);
            } catch (const std::exception &) {
                const std::string tail = r.text.substr(std::min(st->accumulated.size(), r.text.size()));
                if (!tail.empty()) {
                    common_json delta = common_json::object();
                    delta["content"] = tail;
                    sse_write(sink, make_chat_chunk(*st, delta, nullptr));
                }
            }

            const std::string finish = finish_reason_str(r, !st->prev_msg.tool_calls.empty());
            sse_write(sink, make_chat_chunk(*st, common_json::object(), finish.c_str()));

            // final usage frame (empty choices per OpenAI stream_options spec)
            common_json usage_chunk = common_json::object();
            usage_chunk["id"]      = st->request_id;
            usage_chunk["object"]  = "chat.completion.chunk";
            usage_chunk["created"] = (int64_t) st->created;
            usage_chunk["model"]   = st->model;
            usage_chunk["choices"] = common_json::array();
            usage_chunk["usage"]   = usage_json(r);
            usage_chunk["timings"] = timings_json(r);
            sse_write(sink, usage_chunk);

            const char done_frame[] = "data: [DONE]\n\n";
            sink.write(done_frame, sizeof(done_frame) - 1);
            sink.done();
            return true;
        });
    });

    // ---- text completions ------------------------------------------------

    svr.Post("/v1/completions", [&](const httplib::Request & req, httplib::Response & res) {
        const common_json body = common_json::parse(req.body);
        if (engine.embedding_mode()) {
            send_error(res, 501, "this instance serves embeddings only (started with --embedding)", "not_supported_error");
            return;
        }
        if (!body.contains("prompt")) {
            send_error(res, 400, "missing 'prompt'", "invalid_request_error");
            return;
        }

        jx_gen_params gp;
        const common_json & prompt = body.at("prompt");
        if (prompt.is_string()) {
            gp.prompt_tokens = engine.tokenize(prompt.get<std::string>(), /* add_special */ true, /* parse_special */ true);
        } else if (prompt.is_array()) {
            for (size_t i = 0; i < prompt.size(); i++) {
                gp.prompt_tokens.push_back(prompt[i].get<int32_t>());
            }
        } else {
            send_error(res, 400, "'prompt' must be a string or an array of token ids", "invalid_request_error");
            return;
        }

        gp.n_predict = parse_max_tokens(body);
        gp.stop      = parse_stop(body);
        gp.sampling  = parse_sampling(body, args.seed);

        if (body.contains("grammar") && body.at("grammar").is_string()) {
            gp.sampling.grammar = common_grammar(COMMON_GRAMMAR_TYPE_USER, body.at("grammar").get<std::string>());
        } else if (body.contains("json_schema")) {
            gp.sampling.grammar = common_grammar(COMMON_GRAMMAR_TYPE_OUTPUT_FORMAT,
                                                 json_schema_to_grammar(body.at("json_schema")));
        }

        const bool stream = body.value<bool>("stream", false);
        const std::string request_id = gen_request_id("cmpl");
        const std::time_t created    = std::time(nullptr);
        const std::string model      = engine.alias();

        auto make_cmpl_payload = [request_id, created, model](const std::string & text, const char * finish) {
            common_json choice = common_json::object();
            choice["index"] = 0;
            choice["text"]  = text;
            if (finish) {
                choice["finish_reason"] = finish;
            } else {
                choice["finish_reason"] = nullptr;
            }
            common_json choices = common_json::array();
            choices.push_back(choice);

            common_json out = common_json::object();
            out["id"]      = request_id;
            out["object"]  = "text_completion";
            out["created"] = (int64_t) created;
            out["model"]   = model;
            out["choices"] = choices;
            return out;
        };

        if (!stream) {
            jx_gen_result r = engine.generate(gp, nullptr);
            if (!r.error.empty()) {
                send_error(res, 500, r.error, "server_error");
                return;
            }
            common_json out = make_cmpl_payload(r.text, finish_reason_str(r, false).c_str());
            out["usage"]   = usage_json(r);
            out["timings"] = timings_json(r);
            res.set_content(out.dump_safe(), "application/json; charset=utf-8");
            return;
        }

        res.set_chunked_content_provider("text/event-stream", [&engine, gp, make_cmpl_payload](size_t offset, httplib::DataSink & sink) {
            if (offset > 0) {
                return false;
            }

            auto emit_piece = [&](const std::string & piece) -> bool {
                if (piece.empty()) {
                    return sink.is_writable();
                }
                return sse_write(sink, make_cmpl_payload(piece, nullptr));
            };

            jx_gen_result r = engine.generate(gp, emit_piece);

            if (!r.error.empty()) {
                common_json err = common_json::object();
                common_json e   = common_json::object();
                e["message"] = r.error;
                e["type"]    = "server_error";
                err["error"] = e;
                sse_write(sink, err);
                sink.done();
                return true;
            }
            if (r.finish == JX_FINISH_CANCEL) {
                return false;
            }

            common_json final_chunk = make_cmpl_payload("", finish_reason_str(r, false).c_str());
            final_chunk["usage"]   = usage_json(r);
            final_chunk["timings"] = timings_json(r);
            sse_write(sink, final_chunk);

            const char done_frame[] = "data: [DONE]\n\n";
            sink.write(done_frame, sizeof(done_frame) - 1);
            sink.done();
            return true;
        });
    });

    // ---- embeddings ------------------------------------------------------

    svr.Post("/v1/embeddings", [&](const httplib::Request & req, httplib::Response & res) {
        const common_json body = common_json::parse(req.body);
        if (!engine.embedding_mode()) {
            send_error(res, 501, "embeddings are disabled; start jx-engine with --embedding", "not_supported_error");
            return;
        }
        if (!body.contains("input")) {
            send_error(res, 400, "missing 'input'", "invalid_request_error");
            return;
        }

        // input: string | [string] | [int] | [[int]]
        std::vector<std::vector<llama_token>> inputs;
        const common_json & input = body.at("input");
        if (input.is_string()) {
            inputs.push_back(engine.tokenize(input.get<std::string>(), /* add_special */ true, /* parse_special */ false));
        } else if (input.is_array() && input.size() > 0 && input[(size_t) 0].is_number_integer()) {
            std::vector<llama_token> toks;
            for (size_t i = 0; i < input.size(); i++) {
                toks.push_back(input[i].get<int32_t>());
            }
            inputs.push_back(std::move(toks));
        } else if (input.is_array()) {
            for (size_t i = 0; i < input.size(); i++) {
                const common_json & item = input[i];
                if (item.is_string()) {
                    inputs.push_back(engine.tokenize(item.get<std::string>(), true, false));
                } else if (item.is_array()) {
                    std::vector<llama_token> toks;
                    for (size_t k = 0; k < item.size(); k++) {
                        toks.push_back(item[k].get<int32_t>());
                    }
                    inputs.push_back(std::move(toks));
                } else {
                    send_error(res, 400, "'input' array items must be strings or token arrays", "invalid_request_error");
                    return;
                }
            }
        } else {
            send_error(res, 400, "'input' must be a string or an array", "invalid_request_error");
            return;
        }

        common_json data = common_json::array();
        int64_t n_prompt_total = 0;
        for (size_t i = 0; i < inputs.size(); i++) {
            std::string err;
            const std::vector<float> emb = engine.embed(inputs[i], err);
            if (!err.empty()) {
                send_error(res, 500, err, "server_error");
                return;
            }
            n_prompt_total += (int64_t) inputs[i].size();

            common_json vec = common_json::array();
            for (float v : emb) {
                vec.push_back((double) v);
            }
            common_json item = common_json::object();
            item["object"]    = "embedding";
            item["index"]     = (int64_t) i;
            item["embedding"] = vec;
            data.push_back(item);
        }

        common_json usage = common_json::object();
        usage["prompt_tokens"] = n_prompt_total;
        usage["total_tokens"]  = n_prompt_total;

        common_json out = common_json::object();
        out["object"] = "list";
        out["model"]  = engine.alias();
        out["data"]   = data;
        out["usage"]  = usage;
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // ---------------------------------------------------------------------

    svr.set_read_timeout(600);
    svr.set_write_timeout(600);

    if (!svr.bind_to_port(args.host, args.port)) {
        fprintf(stderr, "error: failed to bind to %s:%d\n", args.host.c_str(), args.port);
        return 1;
    }

    fprintf(stderr, "jx-engine listening on http://%s:%d (model: %s, ctx: %u%s)\n",
            args.host.c_str(), args.port, engine.alias().c_str(), engine.n_ctx(),
            engine.embedding_mode() ? ", embedding mode" : "");

    if (!svr.listen_after_bind()) {
        fprintf(stderr, "error: server terminated unexpectedly\n");
        return 1;
    }
    return 0;
}
