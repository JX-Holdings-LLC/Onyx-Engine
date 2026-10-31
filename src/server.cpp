#include "server.h"

#include "http_util.h"
#include "llama_util.h"

#include "onyx_chat.h"
#include "onyx_json.h"
#include "request.h"

#include "httplib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Largest request body accepted; httplib answers 413 beyond it. The same
// figure as httplib's implicit default, written down so it is a documented
// limit rather than a library detail (docs/reference/stability.md).
static constexpr size_t ONYX_MAX_PAYLOAD_BYTES = 100u * 1024u * 1024u;

// Seconds a client told "busy" (--max-queue, --queue-timeout) is asked to wait.
static constexpr int ONYX_RETRY_AFTER_SECONDS = 1;

// Seconds httplib waits for the next bytes of a request before it drops the
// connection. Short, so idle or slow-header clients cannot hold the worker
// threads (from httplib v0.62 it bounds the whole request head). Responses
// keep the long write timeout below: SSE streams can run for minutes.
static constexpr int ONYX_READ_TIMEOUT_SECONDS  = 30;
static constexpr int ONYX_WRITE_TIMEOUT_SECONDS = 600;

// Deepest array/object nesting accepted in a request body; deeper bodies get
// a 400 before they reach the JSON parser or the schema converter, both of
// which recurse once per level.
static constexpr size_t ONYX_MAX_JSON_DEPTH = 64;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static std::string gen_request_id(const char * prefix) {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%016" PRIx64, prefix, (uint64_t) rng());
    return buf;
}

static void send_error(httplib::Response & res, int status, const std::string & message, const char * type) {
    onyx_json err = onyx_json::object();
    onyx_json body = onyx_json::object();
    err["message"] = message;
    err["type"]    = type;
    err["code"]    = status;
    body["error"]  = err;
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

static void send_busy(httplib::Response & res, const std::string & message) {
    res.set_header("Retry-After", std::to_string(ONYX_RETRY_AFTER_SECONDS));
    send_error(res, 503, message, "unavailable_error");
}

// Parses a request body, refusing one nested deeper than ONYX_MAX_JSON_DEPTH
// (std::invalid_argument, which the exception handler answers with 400).
static onyx_json parse_body(const httplib::Request & req) {
    if (json_nesting_depth(req.body) > ONYX_MAX_JSON_DEPTH) {
        throw std::invalid_argument("request body nests arrays and objects deeper than " +
                                    std::to_string(ONYX_MAX_JSON_DEPTH) + " levels");
    }
    return onyx_json::parse(req.body);
}

// The 400 for a response_format / json_schema answer that stopped before its
// JSON was complete: half an object is never returned as if it were output.
static std::string truncated_json_message(const onyx_gen_result & r, int32_t n_predict) {
    const bool by_max_tokens = n_predict >= 0 && r.n_predicted >= n_predict;
    return std::string("the response stopped at ") +
           (by_max_tokens ? "max_tokens (" + std::to_string(n_predict) + ")"
                          : std::string("the context limit")) +
           " before the JSON required by the response format was complete; raise " +
           (by_max_tokens ? "max_tokens" : "--ctx-size or shorten the prompt");
}

static onyx_json truncated_json_frame(const std::string & message) {
    onyx_json e = onyx_json::object();
    e["message"] = message;
    e["type"]    = "invalid_request_error";
    e["code"]    = 400;
    onyx_json err = onyx_json::object();
    err["error"] = e;
    return err;
}

static void send_generation_error(httplib::Response & res, const onyx_gen_result & r) {
    if (r.queue_full || r.queue_timeout) {
        send_busy(res, r.error);
        return;
    }
    const std::string & message = r.error;
    const bool client_error = message.rfind("prompt (", 0) == 0 ||
        message.rfind("prompt contains", 0) == 0 ||
        message.rfind("failed to initialize sampler", 0) == 0;
    send_error(res, client_error ? 400 : 500, message,
               client_error ? "invalid_request_error" : "server_error");
}

// SSE error frame for a generation that failed after the stream started.
static onyx_json stream_error_json(const onyx_gen_result & r) {
    onyx_json e = onyx_json::object();
    e["message"] = r.error;
    const bool busy = r.queue_full || r.queue_timeout;
    e["type"]    = busy ? "unavailable_error" : "server_error";
    e["code"]    = busy ? 503 : 500;
    onyx_json err = onyx_json::object();
    err["error"] = e;
    return err;
}

// Prometheus text exposition of the engine counters under llama-server's
// llamacpp:* names, so dashboards built for llama-server read it unchanged.
static std::string metrics_text(const onyx_engine_metrics & m) {
    std::string out;
    auto add = [&](const char * name, const char * type, const char * help, double value) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6g", value);
        out += std::string("# HELP ") + name + " " + help + "\n";
        out += std::string("# TYPE ") + name + " " + type + "\n";
        out += std::string(name) + " " + buf + "\n";
    };
    add("llamacpp:prompt_tokens_total", "counter", "Number of prompt tokens processed.", (double) m.prompt_tokens);
    add("llamacpp:prompt_seconds_total", "counter", "Prompt process time", m.prompt_seconds);
    add("llamacpp:tokens_predicted_total", "counter", "Number of generation tokens processed.", (double) m.predicted_tokens);
    add("llamacpp:tokens_predicted_seconds_total", "counter", "Predict process time", m.predicted_seconds);
    add("llamacpp:n_decode_total", "counter", "Total number of llama_decode() calls", (double) m.n_decode);
    add("llamacpp:n_tokens_max", "counter", "Largest observed n_tokens.", (double) m.n_tokens_max);
    add("llamacpp:n_busy_slots_per_decode", "counter", "Average number of busy slots per llama_decode() call",
        m.n_decode > 0 ? (double) m.n_busy_slots / (double) m.n_decode : 0.0);
    add("llamacpp:prompt_tokens_seconds", "gauge", "Average prompt throughput in tokens/s.",
        m.prompt_seconds > 0 ? (double) m.prompt_tokens / m.prompt_seconds : 0.0);
    add("llamacpp:predicted_tokens_seconds", "gauge", "Average generation throughput in tokens/s.",
        m.predicted_seconds > 0 ? (double) m.predicted_tokens / m.predicted_seconds : 0.0);
    add("llamacpp:requests_processing", "gauge", "Number of requests processing.", (double) m.requests_processing);
    add("llamacpp:requests_deferred", "gauge", "Number of requests deferred.", (double) m.requests_deferred);
    add("onyx:requests_rejected_total", "counter", "Requests refused with 503 because --max-queue was reached.",
        (double) m.requests_rejected);
    add("onyx:requests_timed_out_total", "counter", "Requests answered 503 after waiting --queue-timeout for a slot.",
        (double) m.requests_timed_out);
    return out;
}

static bool reject_unsupported_fields(const onyx_json & body, httplib::Response & res,
                                      std::initializer_list<const char *> fields) {
    for (const char * field : fields) {
        if (body.contains(field)) {
            send_error(res, 400, std::string("unsupported field: ") + field, "invalid_request_error");
            return true;
        }
    }
    if (body.contains("n") && body.at("n").get<int32_t>() != 1) {
        send_error(res, 400, "only n=1 is supported", "invalid_request_error");
        return true;
    }
    return false;
}

// Builds (and drops) the request's sampler before a stream opens, so a bad
// grammar or penalty is still a 400 rather than an error frame.
static bool validate_stream_sampler(const onyx_engine & engine, const onyx_gen_params & params,
                                    httplib::Response & res) {
    try {
        onyx_sampler sampler(engine.model(), params.sampling);
        return true;
    } catch (const std::exception & e) {
        send_error(res, 400, e.what(), "invalid_request_error");
        return false;
    }
}

static std::string finish_reason_str(const onyx_gen_result & r, bool has_tool_calls) {
    if (r.finish == ONYX_FINISH_LENGTH) {
        return "length";
    }
    return has_tool_calls ? "tool_calls" : "stop";
}

static onyx_json usage_json(const onyx_gen_result & r) {
    onyx_json u = onyx_json::object();
    u["prompt_tokens"]     = r.n_prompt;
    u["completion_tokens"] = r.n_predicted;
    u["total_tokens"]      = r.n_prompt + r.n_predicted;
    return u;
}

static onyx_json timings_json(const onyx_gen_result & r) {
    onyx_json t = onyx_json::object();
    t["cache_n"]              = r.n_cached;
    t["prompt_n"]             = r.n_prompt - r.n_cached;
    t["prompt_ms"]            = r.t_prompt_ms;
    t["prompt_per_second"]    = r.t_prompt_ms > 0 ? (r.n_prompt - r.n_cached) / r.t_prompt_ms * 1000.0 : 0.0;
    t["predicted_n"]          = r.n_predicted;
    t["predicted_ms"]         = r.t_predict_ms;
    t["predicted_per_second"] = r.t_predict_ms > 0 ? r.n_predicted / r.t_predict_ms * 1000.0 : 0.0;
    return t;
}

// ---------------------------------------------------------------------------
// logprobs (v2)
// ---------------------------------------------------------------------------

static onyx_json bytes_json(const std::string & s) {
    onyx_json arr = onyx_json::array();
    for (unsigned char c : s) {
        arr.push_back((int64_t) c);
    }
    return arr;
}

// OpenAI chat-completions logprobs.content[] entry shape, used by both
// /v1/chat/completions (non-streaming and streaming).
static onyx_json chat_logprob_entry(const onyx_token_probs & p) {
    onyx_json e = onyx_json::object();
    e["token"]   = p.sampled.piece;
    e["logprob"] = p.sampled.logprob;
    e["bytes"]   = bytes_json(p.sampled.piece);
    onyx_json top = onyx_json::array();
    for (const auto & t : p.top) {
        onyx_json te = onyx_json::object();
        te["token"]   = t.piece;
        te["logprob"] = t.logprob;
        te["bytes"]   = bytes_json(t.piece);
        top.push_back(te);
    }
    e["top_logprobs"] = top;
    return e;
}

static onyx_json chat_logprobs_content(const std::vector<onyx_token_probs> & probs) {
    onyx_json content = onyx_json::array();
    for (const auto & p : probs) {
        content.push_back(chat_logprob_entry(p));
    }
    onyx_json out = onyx_json::object();
    out["content"] = content;
    return out;
}

// Legacy OpenAI /v1/completions logprobs shape: flat parallel arrays, plus
// `text_offset` relative to the start of the whole returned completion text
// -- `offset` is threaded through by the caller so a streaming response's
// per-frame call keeps accumulating across frames.
static onyx_json legacy_logprobs(const std::vector<onyx_token_probs> & probs, size_t & offset) {
    onyx_json tokens         = onyx_json::array();
    onyx_json token_logprobs = onyx_json::array();
    onyx_json top_logprobs   = onyx_json::array();
    onyx_json text_offset    = onyx_json::array();

    for (const auto & p : probs) {
        tokens.push_back(p.sampled.piece);
        token_logprobs.push_back(p.sampled.logprob);
        onyx_json top = onyx_json::object();
        for (const auto & t : p.top) {
            top[t.piece] = t.logprob;
        }
        top_logprobs.push_back(top);
        text_offset.push_back((int64_t) offset);
        offset += p.sampled.piece.size();
    }

    onyx_json out = onyx_json::object();
    out["tokens"]         = tokens;
    out["token_logprobs"] = token_logprobs;
    out["top_logprobs"]   = top_logprobs;
    out["text_offset"]    = text_offset;
    return out;
}

// ---------------------------------------------------------------------------
// multimodal content parts
// ---------------------------------------------------------------------------

// Rewrites the OpenAI content-part arrays in `messages` in place: every
// image_url / input_audio part is decoded into `out_media` and replaced by a
// media_marker part carrying the mtmd marker text, so the chat template
// renders the marker exactly where the media was and mtmd_tokenize matches
// markers to buffers in order. Throws (-> 400) on anything unacceptable.
static void extract_media(onyx_json & messages, const onyx_engine & engine,
                          std::vector<onyx_media_buffer> & out_media) {
    const std::string marker = engine.media_marker();

    for (auto & msg : messages) {
        if (!msg.is_object() || !msg.contains("content") || !msg.at("content").is_array()) {
            continue;
        }
        for (auto & part : msg.at("content")) {
            if (!part.is_object()) {
                continue;
            }
            const std::string type = part.value<std::string>("type", "");
            if (type != "image_url" && type != "input_audio") {
                continue;
            }

            const bool  is_image = type == "image_url";
            const char * what    = is_image ? "image" : "audio";

            std::string payload;
            if (is_image) {
                const onyx_json & iu = part.at("image_url");
                payload = iu.is_string() ? iu.get<std::string>() : iu.value<std::string>("url", "");
            } else {
                const onyx_json & ia = part.at("input_audio");
                payload = ia.is_string() ? ia.get<std::string>() : ia.value<std::string>("data", "");
            }
            if (payload.empty()) {
                throw std::runtime_error(std::string("'") + type + "' part carries no data");
            }
            reject_media_url(payload, what);

            if (!engine.has_mmproj()) {
                throw std::runtime_error("multimodal input requires a projector; start onyx-engine with --mmproj");
            }
            if (is_image && !engine.supports_vision()) {
                throw std::runtime_error("the loaded projector (--mmproj) does not support image input");
            }
            if (!is_image && !engine.supports_audio()) {
                throw std::runtime_error("the loaded projector (--mmproj) does not support audio input");
            }

            out_media.push_back(decode_media_payload(payload, what));
            part = onyx_json{{"type", "media_marker"}, {"text", marker}};
        }
    }
}

// ---------------------------------------------------------------------------
// request parsing (stop, max tokens and sampling: request.cpp)
// ---------------------------------------------------------------------------

// Builds the engine-side reasoning-budget config for one chat request (v2).
// `cr` is the already-rendered chat request, needed for the template's own
// thinking tags (falling back to "<think>"/"</think>" when the template
// doesn't expose any, per the v2 API reference §4) and for the rendered
// prompt text (to detect a deepseek-style template whose generation prompt
// already ends inside the thinking block).
static onyx_reasoning_budget build_reasoning_budget(const onyx_json & body, const onyx_args & args,
                                                  const onyx_chat_render & cr, const onyx_engine & engine) {
    onyx_reasoning_budget rb;

    int32_t budget = args.reasoning_budget;
    if (body.contains("reasoning_budget_tokens") && !body.at("reasoning_budget_tokens").is_null()) {
        budget = body.at("reasoning_budget_tokens").get<int32_t>();
    }
    if (budget < 0) {
        return rb;   // -1 (or below): unrestricted, feature stays inert
    }

    const std::string start_tag = !cr.thinking_start_tag.empty() ? cr.thinking_start_tag : std::string("<think>");
    const std::string end_tag   = !cr.thinking_end_tag.empty()   ? cr.thinking_end_tag   : std::string("</think>");

    rb.enabled   = true;
    rb.budget    = budget;
    rb.start_tag = engine.tokenize(start_tag, /* add_special */ false, /* parse_special */ true);
    rb.end_tag   = engine.tokenize(end_tag,   /* add_special */ false, /* parse_special */ true);

    // budget 0 means "suppress thinking entirely": force straight to the
    // closing tag with no injected message. budget > 0 injects the
    // configured message before the closing tag once the budget is spent.
    if (budget > 0 && !args.reasoning_budget_message.empty()) {
        rb.forced = engine.tokenize(args.reasoning_budget_message, false, true);
    }
    rb.forced.insert(rb.forced.end(), rb.end_tag.begin(), rb.end_tag.end());

    // deepseek-style templates render a generation prompt that already ends
    // with the start tag (e.g. "...Assistant:<think>"); detect that
    // textually rather than by token match, since prompt-context tokenization
    // of the tag can differ from tokenizing it standalone.
    const std::string prompt = cr.prompt.str();
    rb.start_in_prompt = prompt.size() >= start_tag.size() &&
        prompt.compare(prompt.size() - start_tag.size(), start_tag.size(), start_tag) == 0;

    return rb;
}

// ---------------------------------------------------------------------------
// chunk / response builders (chat)
// ---------------------------------------------------------------------------

struct chat_stream_state {
    std::string                       request_id;
    std::string                       model;
    std::time_t                       created = 0;
    std::unique_ptr<onyx_chat_parser> parser;
    std::string                       accumulated;
};

static onyx_json make_chat_chunk(const chat_stream_state & st, onyx_json delta, const char * finish_reason,
                                   onyx_json logprobs = nullptr) {
    onyx_json choice = onyx_json::object();
    choice["index"] = 0;
    choice["delta"] = delta;
    if (finish_reason) {
        choice["finish_reason"] = finish_reason;
    } else {
        choice["finish_reason"] = nullptr;
    }
    choice["logprobs"] = logprobs.is_null() ? onyx_json(nullptr) : logprobs;
    onyx_json choices = onyx_json::array();
    choices.push_back(choice);

    onyx_json chunk = onyx_json::object();
    chunk["id"]      = st.request_id;
    chunk["object"]  = "chat.completion.chunk";
    chunk["created"] = (int64_t) st.created;
    chunk["model"]   = st.model;
    chunk["choices"] = choices;
    return chunk;
}

static bool sse_write(httplib::DataSink & sink, const onyx_json & payload) {
    const std::string frame = "data: " + onyx_json_dump_safe(payload) + "\n\n";
    return sink.write(frame.data(), frame.size());
}

// ---------------------------------------------------------------------------
// server
// ---------------------------------------------------------------------------

int onyx_server_run(onyx_engine & engine, const onyx_args & args, const std::function<bool()> & load) {
    httplib::Server svr;

    // false until `load` succeeds; every route but /health is refused until then
    std::atomic<bool> ready{false};
    int bound_port = args.port;
    const bool loopback = onyx_host_is_loopback(args.host);

    svr.set_default_headers({
        {"Server", "onyx-engine/" ONYX_ENGINE_VERSION},
    });

    svr.Options(".*", [](const httplib::Request &, httplib::Response & res) {
        res.status = 403;
    });

    // Every request passes here first, in this order: echo X-Request-Id,
    // refuse browsers, refuse a foreign Host on a loopback bind, check the
    // bearer token (not on /health), and answer 503 while the model loads.
    {
        const std::string expected = "Bearer " + args.api_key;
        svr.set_pre_routing_handler([expected, &args, &ready, loopback](const httplib::Request & req, httplib::Response & res) {
            const std::string request_id = req.get_header_value("X-Request-Id");
            if (valid_request_id(request_id)) {
                res.set_header("X-Request-Id", request_id);
            }
            if (req.has_header("Origin")) {
                send_error(res, 403, "browser-origin requests are disabled", "invalid_request_error");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (loopback && req.has_header("Host") && !host_header_is_loopback(req.get_header_value("Host"))) {
                send_error(res, 403, "Host header is not a loopback name; this server is bound to loopback",
                           "invalid_request_error");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.path == "/health" || req.method == "OPTIONS") {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            if (!args.api_key.empty() &&
                !constant_time_equals(req.get_header_value("Authorization"), expected)) {
                send_error(res, 401, "invalid or missing API key", "authentication_error");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (!ready.load()) {
                send_error(res, 503, "Loading model", "unavailable_error");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
    }

    svr.set_exception_handler([](const httplib::Request &, httplib::Response & res, std::exception_ptr ep) {
        std::string msg = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const onyx_json_error & e) {
            send_error(res, 400, e.what(), "invalid_request_error");
            return;
        } catch (const std::invalid_argument & e) {
            send_error(res, 400, e.what(), "invalid_request_error");
            return;
        } catch (const std::exception & e) {
            msg = e.what();
        } catch (...) {
        }
        send_error(res, 500, msg, "server_error");
    });

    // ---- health / diagnostics -------------------------------------------

    // 503 with llama-server's loading body until the model is up, then 200.
    // A refused connection now means the process is not running at all.
    svr.Get("/health", [&ready](const httplib::Request &, httplib::Response & res) {
        if (!ready.load()) {
            send_error(res, 503, "Loading model", "unavailable_error");
            return;
        }
        res.set_content("{\"status\":\"ok\"}", "application/json; charset=utf-8");
    });

    if (args.metrics) {
        svr.Get("/metrics", [&engine](const httplib::Request &, httplib::Response & res) {
            res.set_content(metrics_text(engine.metrics()), "text/plain; version=0.0.4");
        });
    }

    svr.Get("/props", [&engine, &bound_port](const httplib::Request &, httplib::Response & res) {
        onyx_json defaults = onyx_json::object();
        // Per-slot, not the whole context - llama-server parity. Its
        // get_res_props() reports default_generation_settings.n_ctx as
        // meta.slot_n_ctx, which is its own impl->n_ctx_slot(). This is the
        // window a single request is actually measured against (see
        // onyx_engine::n_ctx_slot_ / llama_n_ctx_seq), so it is the number a
        // caller sizing a request needs. The whole context is still reported,
        // as the top-level n_ctx_total below.
        defaults["n_ctx"] = (int64_t) engine.n_ctx_slot();

        onyx_json modalities = onyx_json::object();
        modalities["vision"] = engine.supports_vision();
        modalities["audio"]  = engine.supports_audio();

        onyx_json props = onyx_json::object();
        props["model_alias"]                 = engine.alias();
        props["chat_template"]               = engine.chat_template_source();
        props["build_info"]                  = std::string("onyx-engine/" ONYX_ENGINE_VERSION " (llama.cpp " ONYX_ENGINE_LLAMA_PIN ")");
        // the port actually bound; differs from --port only for --port 0
        props["port"]                        = bound_port;
        // Deliberately the same value as default_generation_settings.n_ctx
        // above: llama-server publishes only the nested one, and consumers
        // differ on which they read (JX Runtime's adapter takes the first of
        // the two that is a positive number), so the two must never disagree.
        props["n_ctx"]                       = (int64_t) engine.n_ctx_slot();
        props["n_ctx_total"]                 = (int64_t) engine.n_ctx();
        props["n_ctx_train"]                 = (int64_t) engine.n_ctx_train();
        props["n_embd"]                      = (int64_t) engine.n_embd();
        props["embedding_mode"]              = engine.embedding_mode();
        props["decision_type"]               = engine.decision_mode()
            ? onyx_json(onyx_decision_type_name(engine.decision().type())) : onyx_json(nullptr);
        props["model_desc"]                  = engine.model_desc();
        props["model_size_bytes"]            = engine.model_size_bytes();
        props["model_n_params"]              = engine.model_n_params();
        props["modalities"]                  = modalities;
        // what prefix reuse can cut back to: "partial" (KV caches), "full"
        // (recurrent state, exact extensions only), "bounded" (recurrent
        // rollback); null outside generation mode
        props["memory_seq_rm"]               = engine.embedding_mode() || engine.decision_mode()
            ? onyx_json(nullptr) : onyx_json(onyx_seq_rm_kind_name(engine.seq_rm()));
        props["default_generation_settings"] = defaults;
        res.set_content(props.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/v1/models", [&](const httplib::Request &, httplib::Response & res) {
        onyx_json meta = onyx_json::object();
        meta["n_ctx_train"] = (int64_t) engine.n_ctx_train();
        meta["n_embd"]      = (int64_t) engine.n_embd();
        meta["size"]        = engine.model_size_bytes();
        meta["n_params"]    = engine.model_n_params();

        onyx_json model = onyx_json::object();
        model["id"]       = engine.alias();
        model["object"]   = "model";
        model["created"]  = (int64_t) std::time(nullptr);
        model["owned_by"] = "onyx-engine";
        model["meta"]     = meta;

        onyx_json data = onyx_json::array();
        data.push_back(model);

        onyx_json out = onyx_json::object();
        out["object"] = "list";
        out["data"]   = data;
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // ---- tokenization ---------------------------------------------------

    svr.Post("/tokenize", [&](const httplib::Request & req, httplib::Response & res) {
        const onyx_json body = parse_body(req);
        const std::string content = body.value<std::string>("content", "");
        const bool add_special    = body.value<bool>("add_special", false);

        const auto tokens = engine.tokenize(content, add_special, true);
        onyx_json toks = onyx_json::array();
        for (llama_token t : tokens) {
            toks.push_back((int64_t) t);
        }
        onyx_json out = onyx_json::object();
        out["tokens"] = toks;
        if (body.value<bool>("with_pieces", false)) {
            onyx_json pieces = onyx_json::array();
            for (llama_token t : tokens) pieces.push_back(onyx_token_to_piece(engine.vocab(), t, true));
            out["pieces"] = pieces;
        }
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/detokenize", [&](const httplib::Request & req, httplib::Response & res) {
        const onyx_json body = parse_body(req);
        std::vector<llama_token> tokens;
        if (body.contains("tokens") && body.at("tokens").is_array()) {
            const onyx_json & arr = body.at("tokens");
            for (size_t i = 0; i < arr.size(); i++) {
                const auto t = arr[i].get<int32_t>();
                if (t < 0 || t >= llama_vocab_n_tokens(engine.vocab())) {
                    send_error(res, 400, "invalid token id", "invalid_request_error");
                    return;
                }
                tokens.push_back(t);
            }
        }
        onyx_json out = onyx_json::object();
        out["content"] = engine.detokenize(tokens, false);
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/apply-template", [&](const httplib::Request & req, httplib::Response & res) {
        const onyx_json body = parse_body(req);
        if (!body.contains("messages")) {
            send_error(res, 400, "missing 'messages'", "invalid_request_error");
            return;
        }
        try {
            const auto cr = engine.chat_templates().render(body, engine.vocab());
            onyx_json out = onyx_json::object();
            out["prompt"] = cr.prompt.str();
            res.set_content(out.dump(), "application/json; charset=utf-8");
        } catch (const std::exception & e) {
            send_error(res, 400, e.what(), "invalid_request_error");
        }
    });

    // ---- chat completions ------------------------------------------------

    svr.Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
        onyx_json body = parse_body(req);
        if (reject_unsupported_fields(body, res, {"logit_bias", "typical_p", "mirostat", "mirostat_tau",
            "mirostat_eta", "dynatemp_range", "dynatemp_exponent", "dry_multiplier", "dry_base",
            "dry_allowed_length", "dry_penalty_last_n", "xtc_probability", "xtc_threshold"})) return;
        if (!body.contains("messages") || !body.at("messages").is_array()) {
            send_error(res, 400, "missing or invalid 'messages'", "invalid_request_error");
            return;
        }
        if (engine.embedding_mode()) {
            send_error(res, 501, "this instance serves embeddings only (started with --embedding)", "not_supported_error");
            return;
        }
        if (engine.decision_mode()) {
            send_error(res, 501, "this instance serves a decision model; use /v1/systemone", "not_supported_error");
            return;
        }

        // decode any image_url / input_audio parts and swap them for mtmd
        // markers before the template renders the prompt
        std::vector<onyx_media_buffer> media;
        try {
            extract_media(body.at("messages"), engine, media);
        } catch (const std::exception & e) {
            send_error(res, 400, e.what(), "invalid_request_error");
            return;
        }

        const bool stream = body.value<bool>("stream", false);

        // logprobs (v2): logprobs:true + top_logprobs:N (OpenAI chat shape).
        const bool include_usage = !body.contains("stream_options") ||
            body.at("stream_options").value<bool>("include_usage", true);
        // top_logprobs without logprobs=true is a hard error, matching
        // llama.cpp's own oaicompat parsing.
        const bool logprobs_requested = body.value<bool>("logprobs", false);
        const bool has_top_logprobs   = body.contains("top_logprobs") && !body.at("top_logprobs").is_null();
        if (has_top_logprobs && !logprobs_requested) {
            send_error(res, 400, "top_logprobs requires logprobs to be set to true", "invalid_request_error");
            return;
        }
        int32_t top_logprobs = 0;
        if (has_top_logprobs) {
            top_logprobs = body.at("top_logprobs").get<int32_t>();
            if (top_logprobs < 0 || top_logprobs > 20) {
                send_error(res, 400, "top_logprobs must be between 0 and 20", "invalid_request_error");
                return;
            }
        }

        onyx_chat_render cr;
        try {
            cr = engine.chat_templates().render(body, engine.vocab());
        } catch (const std::exception & e) {
            send_error(res, 400, std::string("failed to apply chat template: ") + e.what(), "invalid_request_error");
            return;
        }

        onyx_gen_params gp;
        gp.want_logprobs = logprobs_requested;
        gp.n_probs       = top_logprobs;
        gp.reasoning     = build_reasoning_budget(body, args, cr, engine);
        if (media.empty()) {
            gp.prompt_tokens = engine.tokenize_prompt(cr.prompt);
        } else {
            // mtmd tokenizes the rendered text itself, splitting it on the
            // markers it now contains
            gp.prompt_text = cr.prompt.str();
            gp.media       = std::move(media);
        }
        gp.n_predict     = onyx_parse_max_tokens(body);
        gp.stop          = onyx_parse_stop(body);
        for (const auto & s : cr.additional_stops) {
            gp.stop.push_back(s);
        }
        gp.sampling = onyx_parse_sampling(body, args.seed);

        cr.apply_sampling(gp.sampling);
        // response_format / json_schema: a length cut leaves invalid JSON, so
        // it is answered with an error instead of half an object
        const bool json_output = cr.output_format;
        if (gp.reasoning.enabled && gp.reasoning.budget >= 0 &&
            !gp.sampling.grammar.empty() && !gp.sampling.grammar_lazy) {
            send_error(res, 400, "reasoning budget cannot force tokens through a non-lazy grammar",
                       "invalid_request_error");
            return;
        }
        if (gp.media.empty() && gp.prompt_tokens.size() >= engine.n_ctx_slot()) {
            send_error(res, 400, "prompt does not fit in the context window", "invalid_request_error");
            return;
        }

        const std::string request_id = gen_request_id("chatcmpl");
        const std::time_t created    = std::time(nullptr);

        if (!stream) {
            onyx_gen_result r = engine.generate(gp, [&req](const std::string &, const std::vector<onyx_token_probs> &) {
                return !req.is_connection_closed();
            });
            if (!r.error.empty()) {
                send_generation_error(res, r);
                return;
            }
            if (json_output && r.finish == ONYX_FINISH_LENGTH) {
                send_error(res, 400, truncated_json_message(r, gp.n_predict), "invalid_request_error");
                return;
            }

            auto parser = cr.make_parser();
            onyx_json message = parser->message(r.text);
            onyx_json choice  = onyx_json::object();
            choice["index"]         = 0;
            choice["message"]       = message;
            choice["finish_reason"] = finish_reason_str(r, parser->has_tool_calls());
            choice["logprobs"]      = gp.want_logprobs ? chat_logprobs_content(r.probs) : onyx_json(nullptr);
            onyx_json choices = onyx_json::array();
            choices.push_back(choice);

            onyx_json out = onyx_json::object();
            out["id"]      = request_id;
            out["object"]  = "chat.completion";
            out["created"] = (int64_t) created;
            out["model"]   = engine.alias();
            out["choices"] = choices;
            out["usage"]   = usage_json(r);
            out["timings"] = timings_json(r);
            out["truncated"] = r.truncated;
            res.set_content(onyx_json_dump_safe(out), "application/json; charset=utf-8");
            return;
        }

        // streaming
        auto st = std::make_shared<chat_stream_state>();
        st->request_id    = request_id;
        st->model         = engine.alias();
        st->created       = created;
        st->parser        = cr.make_parser();

        if (!validate_stream_sampler(engine, gp, res)) return;
        if (engine.queue_full()) {
            send_busy(res, "server is busy: the request queue is full (--max-queue)");
            return;
        }
        res.set_chunked_content_provider("text/event-stream", [&engine, gp, st, include_usage, json_output](size_t offset, httplib::DataSink & sink) {
            if (offset > 0) {
                // single-shot provider: everything is written on the first call
                return false;
            }

            {
                onyx_json role_delta = onyx_json::object();
                role_delta["role"]    = "assistant";
                role_delta["content"] = "";
                if (!sse_write(sink, make_chat_chunk(*st, role_delta, nullptr))) {
                    return false;
                }
            }

            auto emit_piece = [&](const std::string & piece, const std::vector<onyx_token_probs> & probs) -> bool {
                // logprobs travel independently of the parsed-message delta
                // diffing below: token/piece alignment with tool-call deltas
                // is meaningless once the parser has reshaped the text, so
                // (matching that OpenAI itself does not tie the two
                // together) each piece's logprobs go out as their own frame,
                // with an empty delta, the moment that piece's tokens exist.
                if (gp.want_logprobs && !probs.empty()) {
                    if (!sse_write(sink, make_chat_chunk(*st, onyx_json::object(), nullptr, chat_logprobs_content(probs)))) {
                        return false;
                    }
                }

                st->accumulated += piece;
                std::vector<onyx_json> deltas;
                if (!st->parser->update(st->accumulated, /* is_partial */ true, deltas)) {
                    // partial output not yet parseable - hold until more arrives
                    return sink.is_writable();
                }
                for (const auto & delta : deltas) {
                    if (!sse_write(sink, make_chat_chunk(*st, delta, nullptr))) {
                        return false;
                    }
                }
                return true;
            };

            onyx_gen_result r = engine.generate(gp, emit_piece);

            if (!r.error.empty()) {
                sse_write(sink, stream_error_json(r));
                sink.done();
                return true;
            }
            if (r.finish == ONYX_FINISH_CANCEL) {
                return false;
            }
            if (json_output && r.finish == ONYX_FINISH_LENGTH) {
                // the deltas so far are not valid JSON; end on an error frame
                // in place of the finish, usage and [DONE] frames
                sse_write(sink, truncated_json_frame(truncated_json_message(r, gp.n_predict)));
                sink.done();
                return true;
            }

            // flush any tail withheld by the partial parser
            std::vector<onyx_json> deltas;
            if (st->parser->update(r.text, /* is_partial */ false, deltas)) {
                for (const auto & delta : deltas) {
                    sse_write(sink, make_chat_chunk(*st, delta, nullptr));
                }
            } else {
                const std::string tail = r.text.substr(std::min(st->accumulated.size(), r.text.size()));
                if (!tail.empty()) {
                    onyx_json delta = onyx_json::object();
                    delta["content"] = tail;
                    sse_write(sink, make_chat_chunk(*st, delta, nullptr));
                }
            }

            const std::string finish = finish_reason_str(r, st->parser->has_tool_calls());
            sse_write(sink, make_chat_chunk(*st, onyx_json::object(), finish.c_str()));

            // final usage frame (empty choices per OpenAI stream_options spec)
            onyx_json usage_chunk = onyx_json::object();
            usage_chunk["id"]      = st->request_id;
            usage_chunk["object"]  = "chat.completion.chunk";
            usage_chunk["created"] = (int64_t) st->created;
            usage_chunk["model"]   = st->model;
            usage_chunk["choices"] = onyx_json::array();
            usage_chunk["usage"]   = usage_json(r);
            usage_chunk["timings"] = timings_json(r);
            usage_chunk["truncated"] = r.truncated;
            if (include_usage) sse_write(sink, usage_chunk);

            const char done_frame[] = "data: [DONE]\n\n";
            sink.write(done_frame, sizeof(done_frame) - 1);
            sink.done();
            return true;
        });
    });

    // ---- text completions ------------------------------------------------

    svr.Post("/v1/completions", [&](const httplib::Request & req, httplib::Response & res) {
        const onyx_json body = parse_body(req);
        if (reject_unsupported_fields(body, res, {"logit_bias", "echo", "suffix", "best_of",
            "typical_p", "mirostat", "mirostat_tau", "mirostat_eta", "dynatemp_range",
            "dynatemp_exponent", "dry_multiplier", "dry_base", "dry_allowed_length",
            "dry_penalty_last_n", "xtc_probability", "xtc_threshold"})) return;
        if (engine.embedding_mode()) {
            send_error(res, 501, "this instance serves embeddings only (started with --embedding)", "not_supported_error");
            return;
        }
        if (engine.decision_mode()) {
            send_error(res, 501, "this instance serves a decision model; use /v1/systemone", "not_supported_error");
            return;
        }
        if (!body.contains("prompt")) {
            send_error(res, 400, "missing 'prompt'", "invalid_request_error");
            return;
        }

        onyx_gen_params gp;
        const onyx_json & prompt = body.at("prompt");
        if (prompt.is_string()) {
            gp.prompt_tokens = engine.tokenize(prompt.get<std::string>(), /* add_special */ true, /* parse_special */ true);
        } else if (prompt.is_array()) {
            for (size_t i = 0; i < prompt.size(); i++) {
                const auto t = prompt[i].get<int32_t>();
                if (t < 0 || t >= llama_vocab_n_tokens(engine.vocab())) {
                    send_error(res, 400, "invalid token id", "invalid_request_error");
                    return;
                }
                gp.prompt_tokens.push_back(t);
            }
        } else {
            send_error(res, 400, "'prompt' must be a string or an array of token ids", "invalid_request_error");
            return;
        }

        gp.n_predict = onyx_parse_max_tokens(body);
        gp.stop      = onyx_parse_stop(body);
        gp.sampling  = onyx_parse_sampling(body, args.seed);

        // logprobs (v2, legacy OpenAI /v1/completions form): an integer N,
        // 0..20, meaning "report the top N alternatives per token" (the
        // sampled token's own logprob is always included once requested).
        if (body.contains("logprobs") && !body.at("logprobs").is_null()) {
            const int32_t n = body.at("logprobs").get<int32_t>();
            if (n < 0 || n > 20) {
                send_error(res, 400, "logprobs must be between 0 and 20", "invalid_request_error");
                return;
            }
            gp.want_logprobs = true;
            gp.n_probs       = n;
        }

        if (body.contains("grammar") && body.at("grammar").is_string()) {
            gp.sampling.grammar_kind = ONYX_GRAMMAR_USER;
            gp.sampling.grammar      = body.at("grammar").get<std::string>();
        } else if (body.contains("json_schema")) {
            try {
                gp.sampling.grammar_kind = ONYX_GRAMMAR_OUTPUT_FORMAT;
                gp.sampling.grammar      = onyx_json_schema_to_grammar(body.at("json_schema"));
            } catch (const std::exception & e) {
                send_error(res, 400, e.what(), "invalid_request_error");
                return;
            }
        }

        // a json_schema answer cut by length is an error, not half an object
        const bool json_output = gp.sampling.grammar_kind == ONYX_GRAMMAR_OUTPUT_FORMAT;

        const bool stream = body.value<bool>("stream", false);
        const bool include_usage = !body.contains("stream_options") ||
            body.at("stream_options").value<bool>("include_usage", true);
        if (gp.prompt_tokens.size() >= engine.n_ctx_slot()) {
            send_error(res, 400, "prompt does not fit in the context window", "invalid_request_error");
            return;
        }
        const std::string request_id = gen_request_id("cmpl");
        const std::time_t created    = std::time(nullptr);
        const std::string model      = engine.alias();

        auto make_cmpl_payload = [request_id, created, model](const std::string & text, const char * finish,
                                                              onyx_json logprobs = nullptr) {
            onyx_json choice = onyx_json::object();
            choice["index"] = 0;
            choice["text"]  = text;
            if (finish) {
                choice["finish_reason"] = finish;
            } else {
                choice["finish_reason"] = nullptr;
            }
            choice["logprobs"] = logprobs.is_null() ? onyx_json(nullptr) : logprobs;
            onyx_json choices = onyx_json::array();
            choices.push_back(choice);

            onyx_json out = onyx_json::object();
            out["id"]      = request_id;
            out["object"]  = "text_completion";
            out["created"] = (int64_t) created;
            out["model"]   = model;
            out["choices"] = choices;
            return out;
        };

        if (!stream) {
            onyx_gen_result r = engine.generate(gp, [&req](const std::string &, const std::vector<onyx_token_probs> &) {
                return !req.is_connection_closed();
            });
            if (!r.error.empty()) {
                send_generation_error(res, r);
                return;
            }
            if (json_output && r.finish == ONYX_FINISH_LENGTH) {
                send_error(res, 400, truncated_json_message(r, gp.n_predict), "invalid_request_error");
                return;
            }
            onyx_json out = make_cmpl_payload(r.text, finish_reason_str(r, false).c_str(),
                                                gp.want_logprobs ? [&] {
                                                    size_t offset = 0;
                                                    return legacy_logprobs(r.probs, offset);
                                                }() : onyx_json(nullptr));
            out["usage"]   = usage_json(r);
            out["timings"] = timings_json(r);
            out["truncated"] = r.truncated;
            res.set_content(onyx_json_dump_safe(out), "application/json; charset=utf-8");
            return;
        }

        if (!validate_stream_sampler(engine, gp, res)) return;
        if (engine.queue_full()) {
            send_busy(res, "server is busy: the request queue is full (--max-queue)");
            return;
        }
        res.set_chunked_content_provider("text/event-stream", [&engine, gp, make_cmpl_payload, include_usage, json_output](size_t offset, httplib::DataSink & sink) {
            if (offset > 0) {
                return false;
            }

            auto lp_offset = std::make_shared<size_t>(0);
            auto emit_piece = [&, lp_offset](const std::string & piece, const std::vector<onyx_token_probs> & probs) -> bool {
                if (piece.empty() && probs.empty()) {
                    return sink.is_writable();
                }
                onyx_json payload = make_cmpl_payload(piece, nullptr,
                    gp.want_logprobs && !probs.empty() ? legacy_logprobs(probs, *lp_offset) : onyx_json(nullptr));
                return sse_write(sink, payload);
            };

            onyx_gen_result r = engine.generate(gp, emit_piece);

            if (!r.error.empty()) {
                sse_write(sink, stream_error_json(r));
                sink.done();
                return true;
            }
            if (r.finish == ONYX_FINISH_CANCEL) {
                return false;
            }
            if (json_output && r.finish == ONYX_FINISH_LENGTH) {
                sse_write(sink, truncated_json_frame(truncated_json_message(r, gp.n_predict)));
                sink.done();
                return true;
            }

            onyx_json final_chunk = make_cmpl_payload("", finish_reason_str(r, false).c_str());
            if (include_usage) final_chunk["usage"] = usage_json(r);
            final_chunk["timings"] = timings_json(r);
            final_chunk["truncated"] = r.truncated;
            sse_write(sink, final_chunk);

            const char done_frame[] = "data: [DONE]\n\n";
            sink.write(done_frame, sizeof(done_frame) - 1);
            sink.done();
            return true;
        });
    });

    // ---- embeddings ------------------------------------------------------

    svr.Post("/v1/embeddings", [&](const httplib::Request & req, httplib::Response & res) {
        const onyx_json body = parse_body(req);
        // OpenAI's Matryoshka truncation: keep the first N values, renormalise
        size_t dimensions = 0;
        if (body.contains("dimensions") && !body.at("dimensions").is_null()) {
            const onyx_json & d = body.at("dimensions");
            const int64_t n = d.is_number_integer() ? d.get<int64_t>() : 0;
            if (n < 1 || n > (int64_t) engine.n_embd()) {
                send_error(res, 400, "dimensions must be an integer from 1 to " + std::to_string(engine.n_embd()),
                           "invalid_request_error");
                return;
            }
            if (engine.rank_pooling()) {
                send_error(res, 400, "dimensions does not apply to --pooling rank (scores, not embeddings)",
                           "invalid_request_error");
                return;
            }
            dimensions = (size_t) n;
        }
        // the official OpenAI clients ask for base64 unless told otherwise
        const std::string encoding = body.contains("encoding_format") && !body.at("encoding_format").is_null()
            ? body.at("encoding_format").get<std::string>() : "float";
        if (encoding != "float" && encoding != "base64") {
            send_error(res, 400, "encoding_format must be float or base64", "invalid_request_error");
            return;
        }
        if (!engine.embedding_mode()) {
            send_error(res, 501, "embeddings are disabled; start onyx-engine with --embedding", "not_supported_error");
            return;
        }
        if (!body.contains("input")) {
            send_error(res, 400, "missing 'input'", "invalid_request_error");
            return;
        }

        // input: string | [string] | [int] | [[int]]
        std::vector<std::vector<llama_token>> inputs;
        const onyx_json & input = body.at("input");
        if (input.is_string()) {
            inputs.push_back(engine.tokenize(input.get<std::string>(), /* add_special */ true, /* parse_special */ false));
        } else if (input.is_array() && input.size() > 0 && input[(size_t) 0].is_number_integer()) {
            std::vector<llama_token> toks;
            for (size_t i = 0; i < input.size(); i++) {
                const auto t = input[i].get<int32_t>();
                if (t < 0 || t >= llama_vocab_n_tokens(engine.vocab())) {
                    send_error(res, 400, "invalid token id", "invalid_request_error");
                    return;
                }
                toks.push_back(t);
            }
            inputs.push_back(std::move(toks));
        } else if (input.is_array()) {
            for (size_t i = 0; i < input.size(); i++) {
                const onyx_json & item = input[i];
                if (item.is_string()) {
                    inputs.push_back(engine.tokenize(item.get<std::string>(), true, false));
                } else if (item.is_array()) {
                    std::vector<llama_token> toks;
                    for (size_t k = 0; k < item.size(); k++) {
                        const auto t = item[k].get<int32_t>();
                        if (t < 0 || t >= llama_vocab_n_tokens(engine.vocab())) {
                            send_error(res, 400, "invalid token id", "invalid_request_error");
                            return;
                        }
                        toks.push_back(t);
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

        onyx_json data = onyx_json::array();
        int64_t n_prompt_total = 0;
        for (size_t i = 0; i < inputs.size(); i++) {
            std::string err;
            std::vector<float> emb = engine.embed(inputs[i], err);
            if (!err.empty()) {
                const bool client_error = err.rfind("input", 0) == 0;
                send_error(res, client_error ? 400 : 500, err,
                           client_error ? "invalid_request_error" : "server_error");
                return;
            }
            if (dimensions > 0) {
                onyx_truncate_embedding(emb, dimensions);
            }
            n_prompt_total += (int64_t) inputs[i].size();

            onyx_json item = onyx_json::object();
            item["object"] = "embedding";
            item["index"]  = (int64_t) i;
            if (encoding == "base64") {
                item["embedding"] = embedding_base64(emb);
            } else {
                onyx_json vec = onyx_json::array();
                for (float v : emb) {
                    vec.push_back((double) v);
                }
                item["embedding"] = vec;
            }
            data.push_back(item);
        }

        onyx_json usage = onyx_json::object();
        usage["prompt_tokens"] = n_prompt_total;
        usage["total_tokens"]  = n_prompt_total;

        onyx_json out = onyx_json::object();
        out["object"] = "list";
        out["model"]  = engine.alias();
        out["data"]   = data;
        out["usage"]  = usage;
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // ---- decision models -------------------------------------------------

    // TypeSafe-compatible System One API: typed questions about a state,
    // answered from one prefill per question (see decision.h).
    svr.Post("/v1/systemone", [&](const httplib::Request & req, httplib::Response & res) {
        const onyx_json body = parse_body(req);
        if (!engine.decision_mode()) {
            send_error(res, 501, "this model is not a decision model", "not_supported_error");
            return;
        }
        const onyx_decision & decision = engine.decision();

        std::vector<onyx_decision_question> questions;
        std::vector<onyx_media_buffer>      files;
        onyx_json                         state;
        try {
            questions = decision.parse_questions(body);
            state     = decision.prepare_state(decision.parse_state(body, files, [](const std::string & url) {
                try {
                    reject_media_url(url, "image");
                    return decode_media_payload(url, "image");
                } catch (const std::runtime_error & e) {
                    throw std::invalid_argument(e.what());
                }
            }));
        } catch (const std::invalid_argument & e) {
            send_error(res, 400, e.what(), "invalid_request_error");
            return;
        }
        if (!files.empty() && (!decision.can_use_images() || !engine.has_mmproj())) {
            send_error(res, 501, "this server does not support image input for decisions; for a model that "
                       "supports it, start onyx-engine with --mmproj", "not_supported_error");
            return;
        }
        if (!files.empty() && !decision.onyx_supports_media()) {
            send_error(res, 501, std::string("image input for ") + onyx_decision_type_name(decision.type()) +
                       " decision models is not supported by onyx-engine yet", "not_supported_error");
            return;
        }

        // one prompt per variant of each question, or one for all of them
        std::vector<onyx_decision_prompt> prompts;
        try {
            if (decision.is_joint()) {
                prompts.push_back(decision.fill_prompt_joint(state, questions, files));
            } else {
                for (const auto & question : questions) {
                    for (size_t variant = 0; variant < decision.n_variants(question); variant++) {
                        prompts.push_back(decision.fill_prompt(state, questions, question, variant, files));
                    }
                }
            }
        } catch (const std::invalid_argument & e) {
            send_error(res, 400, e.what(), "invalid_request_error");
            return;
        }

        std::vector<std::vector<float>> scores;
        int32_t     n_tokens = 0;
        std::string err;
        if (!engine.decide(prompts, decision.can_share_prompt(), scores, n_tokens, err)) {
            const bool client_error = err.rfind("prompt", 0) == 0;
            send_error(res, client_error ? 400 : 500, err, client_error ? "invalid_request_error" : "server_error");
            return;
        }

        onyx_json answers = onyx_json::object();
        size_t i_prompt = 0;
        size_t i_score  = 0;
        for (const auto & question : questions) {
            std::vector<std::vector<float>> question_scores;
            if (decision.is_joint()) {
                // one result with the scores of all the questions, in order
                const auto & all = scores[0];
                if (i_score + question.options.size() > all.size()) {
                    send_error(res, 500, "decision result does not match the number of options", "server_error");
                    return;
                }
                question_scores.emplace_back(all.begin() + i_score, all.begin() + i_score + question.options.size());
                i_score += question.options.size();
            } else {
                for (size_t variant = 0; variant < decision.n_variants(question); variant++) {
                    question_scores.push_back(scores[i_prompt++]);
                }
            }
            answers[question.id] = decision.format_answer(question, question_scores, !files.empty());
        }

        onyx_json usage = onyx_json::object();
        usage["input_tokens"]  = (int64_t) n_tokens;
        usage["output_tokens"] = 0;

        onyx_json out = onyx_json::object();
        out["model"]   = engine.alias();
        out["answers"] = answers;
        out["usage"]   = usage;
        res.set_content(onyx_json_dump_safe(out), "application/json; charset=utf-8");
    });

    // ---------------------------------------------------------------------

    svr.set_read_timeout(ONYX_READ_TIMEOUT_SECONDS);
    svr.set_write_timeout(ONYX_WRITE_TIMEOUT_SECONDS);
    svr.set_payload_max_length(ONYX_MAX_PAYLOAD_BYTES);

    if (args.port == 0) {
        bound_port = svr.bind_to_any_port(args.host);
        if (bound_port <= 0) {
            fprintf(stderr, "error: failed to bind to %s on any port\n", args.host.c_str());
            return 1;
        }
    } else if (!svr.bind_to_port(args.host, args.port)) {
        fprintf(stderr, "error: failed to bind to %s:%d\n", args.host.c_str(), args.port);
        return 1;
    }

    // Serve from a second thread while the model loads on this one.
    std::atomic<bool> listen_failed{false};
    std::thread listener([&svr, &listen_failed] {
        if (!svr.listen_after_bind()) {
            listen_failed = true;
        }
    });
    svr.wait_until_ready();

    fprintf(stderr, "onyx-engine listening on http://%s:%d (loading)\n", args.host.c_str(), bound_port);

    if (!load()) {
        svr.stop();
        listener.join();
        return 1;
    }
    ready = true;

    fprintf(stderr, "onyx-engine ready on http://%s:%d (model: %s, ctx: %u%s)\n",
            args.host.c_str(), bound_port, engine.alias().c_str(), engine.n_ctx(),
            engine.embedding_mode() ? ", embedding mode" : engine.decision_mode() ? ", decision model" : "");

    listener.join();
    if (listen_failed) {
        fprintf(stderr, "error: server terminated unexpectedly\n");
        return 1;
    }
    return 0;
}
