#include "decision.h"
#include "llama_util.h"

#include "onyx_chat.h"

#include "common.h"   // string_format, string_split and the other string helpers

// staging API, the only way to reach these: llama_decision_order and
// llama_model_decision_label (see CMakeLists.txt for the include path)
#include "llama-ext.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

using json = common_json;

// ---------------------------------------------------------------------------
// type detection
// ---------------------------------------------------------------------------

static const struct {
    onyx_decision_type type;
    const char *       name;
} DECISION_TYPE_NAMES[] = {
    { ONYX_DECISION_OPENJEV,        "openjev"        },
    { ONYX_DECISION_LEV,            "lev"            },
    { ONYX_DECISION_KEV,            "kev"            },
    { ONYX_DECISION_NIMBLE,         "nimble"         },
    { ONYX_DECISION_LAYA,           "laya"           },
    { ONYX_DECISION_CLEF,           "clef"           },
    { ONYX_DECISION_PPLX_DECIDER,   "pplx-decider"   },
    { ONYX_DECISION_VLLM_DECISION2, "vllm-decision2" },
    { ONYX_DECISION_VLLM_D3,        "vllm-d3"        },
    { ONYX_DECISION_LFM2_D1,        "lfm2-d1"        },
    { ONYX_DECISION_LFM2_D1_OMNI,   "lfm2-d1-omni"   },
    { ONYX_DECISION_GRANITE_SWITCH, "granite-switch" },
    { ONYX_DECISION_H2O_LIGHTNING,  "h2o-lightning"  },
};

static std::string meta_str(const llama_model * model, const std::string & key) {
    char buf[256];
    const int32_t n = llama_model_meta_val_str(model, key.c_str(), buf, sizeof(buf));
    return n < 0 ? "" : std::string(buf);
}

onyx_decision_type onyx_decision_type_of(const llama_model * model) {
    const std::string arch = meta_str(model, "general.architecture");
    if (arch.empty()) {
        return ONYX_DECISION_NONE;
    }
    char buf[64];
    if (llama_model_meta_val_str(model, (arch + ".decision.type").c_str(), buf, sizeof(buf)) < 0) {
        return ONYX_DECISION_NONE;
    }
    for (const auto & entry : DECISION_TYPE_NAMES) {
        if (entry.name == std::string(buf)) {
            return entry.type;
        }
    }
    return ONYX_DECISION_UNKNOWN;
}

const char * onyx_decision_type_name(onyx_decision_type type) {
    for (const auto & entry : DECISION_TYPE_NAMES) {
        if (entry.type == type) {
            return entry.name;
        }
    }
    return type == ONYX_DECISION_NONE ? "none" : "unknown";
}

bool onyx_decision_reads_embeddings(onyx_decision_type type) {
    switch (type) {
        case ONYX_DECISION_LAYA:
        case ONYX_DECISION_KEV:
        case ONYX_DECISION_CLEF:
        case ONYX_DECISION_LFM2_D1_OMNI:
        case ONYX_DECISION_VLLM_DECISION2:
            return true;
        default:
            return false;
    }
}

int32_t onyx_decision_prompt::pos_first() const {
    int32_t pos = pointer;
    for (const int32_t marker : markers) {
        pos = pos < 0 ? marker : std::min(pos, marker);
    }
    for (size_t i = 0; i < order.size(); i++) {
        if (order[i] != LLAMA_DECISION_ORDER_NONE) {
            return pos < 0 ? (int32_t) i : std::min(pos, (int32_t) i);
        }
    }
    return pos;
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static const char * question_type_name(onyx_decision_question_type type) {
    switch (type) {
        case ONYX_DECISION_QUESTION_CHOICE: return "choice";
        case ONYX_DECISION_QUESTION_SCORE:  return "score";
        case ONYX_DECISION_QUESTION_NOUL:   return "noul";
    }
    return "";
}

// a string field, `def` if missing or null
static std::string str_field(const json & obj, const char * key, const std::string & def) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return def;
    }
    if (!obj.at(key).is_string()) {
        throw std::invalid_argument(std::string("\"") + key + "\" must be a string");
    }
    return obj.at(key).get<std::string>();
}

static bool is_payload(const json & val) {
    return val.is_string() || val.is_object() || val.is_array();
}

// lev reads noul from a rating scale: 0 = certainly no, 8 = certainly yes
static const size_t LEV_N_RATINGS = 9;

// h2o-lightning follows serve_config.json and h2o_lightning_shim.py of the
// model repo: a longer state keeps its head and its tail
static const size_t H2O_MAX_STATE_TOKENS = 32000;

// most media one request takes
static const size_t MAX_MEDIA = 8;

// ---------------------------------------------------------------------------
// model setup
// ---------------------------------------------------------------------------

onyx_decision::onyx_decision()  = default;
onyx_decision::~onyx_decision() = default;

std::vector<llama_token> onyx_decision::tokenize(const std::string & text, bool parse_special) const {
    return onyx_tokenize(vocab_, text, /* add_special */ false, parse_special);
}

void onyx_decision::init(const llama_model * model, const std::string & media_marker) {
    const onyx_decision_type model_type = onyx_decision_type_of(model);
    if (model_type == ONYX_DECISION_NONE) {
        return;
    }

    const std::string prefix    = meta_str(model, "general.architecture") + ".decision.";
    const std::string type_name = meta_str(model, prefix + "type");
    if (model_type == ONYX_DECISION_UNKNOWN) {
        throw std::runtime_error("unsupported decision model type: " + type_name);
    }

    vocab_        = llama_model_get_vocab(model);
    media_marker_ = media_marker;

    const char * tmpl_src = llama_model_chat_template(model, "systemone");
    if (tmpl_src == nullptr) {
        throw std::runtime_error("decision model has no \"systemone\" template");
    }
    tmpl_ = std::make_unique<onyx_jinja_template>(tmpl_src);

    // "<prefix>temperature.<type>[.<bucket>]" and "<prefix>score_bias.<n_levels>.<level>"
    const std::string prefix_temp = prefix + "temperature.";
    const std::string prefix_bias = prefix + "score_bias.";
    for (int32_t i = 0; i < llama_model_meta_count(model); i++) {
        char key[256];
        char val[64];
        if (llama_model_meta_key_by_index(model, i, key, sizeof(key)) < 0 ||
            llama_model_meta_val_str_by_index(model, i, val, sizeof(val)) < 0) {
            continue;
        }
        if (string_starts_with(key, prefix_temp)) {
            const float temp = std::strtof(val, nullptr);
            if (!(temp > 0.0f)) {
                throw std::runtime_error(string_format("invalid decision temperature: %s = %s", key, val));
            }
            temperatures_[key + prefix_temp.size()] = temp;
        } else if (string_starts_with(key, prefix_bias)) {
            const auto   parts = string_split<std::string>(key + prefix_bias.size(), '.');
            const size_t n     = parts.size() == 2 ? std::strtoul(parts[0].c_str(), nullptr, 10) : 0;
            const size_t level = parts.size() == 2 ? std::strtoul(parts[1].c_str(), nullptr, 10) : 0;
            if (n < 2 || n > 255 || level >= n) {
                throw std::runtime_error(string_format("invalid decision score bias: %s", key));
            }
            auto & bias = score_bias_[n];
            bias.resize(n, 0.0f);
            bias[level] = std::strtof(val, nullptr);
        }
    }

    switch (model_type) {
        case ONYX_DECISION_OPENJEV: {
            // one letter per option, each must be a single token
            const std::string letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
            for (const char c : letters) {
                const auto toks = tokenize(std::string(1, c), false);
                if (toks.size() != 1) {
                    throw std::runtime_error(string_format("decision label '%c' is not a single token", c));
                }
                labels_.push_back(toks[0]);
            }
            n_options_max_   = labels_.size();
            noul_true_first_ = true;
            break;
        }
        case ONYX_DECISION_LEV:
        case ONYX_DECISION_GRANITE_SWITCH:
        case ONYX_DECISION_NIMBLE:
        case ONYX_DECISION_PPLX_DECIDER:
        case ONYX_DECISION_VLLM_D3: {
            // label codes are A..Z then AA..ZZ; only the single-token ones are used
            std::vector<std::string> codes;
            for (char a = 'A'; a <= 'Z'; a++) {
                codes.push_back(std::string(1, a));
            }
            for (char a = 'A'; a <= 'Z'; a++) {
                for (char b = 'A'; b <= 'Z'; b++) {
                    codes.push_back(std::string{ a, b });
                }
            }
            for (const auto & code : codes) {
                const auto toks = tokenize(code, false);
                if (toks.size() == 1 && labels_.size() < 255) {
                    labels_.push_back(toks[0]);
                    label_texts_.push_back(code);
                }
            }
            n_options_max_    = labels_.size();
            canonical_legend_ = model_type == ONYX_DECISION_VLLM_D3;
            noul_true_first_  = model_type == ONYX_DECISION_GRANITE_SWITCH;
            break;
        }
        case ONYX_DECISION_KEV: {
            // the hidden state of an option is read at the token that ends it
            const auto toks = tokenize("<|box_end|>", true);
            if (toks.size() != 1) {
                throw std::runtime_error("decision model has no <|box_end|> token");
            }
            token_marker_  = toks[0];
            n_options_max_ = 255;
            break;
        }
        case ONYX_DECISION_LAYA: {
            token_marker_ = llama_vocab_mask(vocab_);
            token_sep_    = llama_vocab_sep(vocab_);
            if (token_marker_ == LLAMA_TOKEN_NULL || token_sep_ == LLAMA_TOKEN_NULL) {
                throw std::runtime_error("decision model has no mask or sep token");
            }
            text_marker_     = onyx_token_to_piece(vocab_, token_marker_, true);
            max_head_tokens_ = std::strtoul(meta_str(model, prefix + "max_head_tokens").c_str(), nullptr, 10);
            if (max_head_tokens_ == 0) {
                throw std::runtime_error("decision model has no valid max_head_tokens");
            }
            n_options_max_ = 255;
            break;
        }
        case ONYX_DECISION_CLEF:
            n_options_max_   = 255;
            noul_true_first_ = true;
            choice_sorted_   = true;
            break;
        case ONYX_DECISION_VLLM_DECISION2:
            n_options_max_       = 255;
            noul_criteria_order_ = true;
            strict_payload_      = true;
            canonical_legend_    = true;
            break;
        case ONYX_DECISION_LFM2_D1:
            n_options_max_   = 255;
            noul_true_first_ = true;
            break;
        case ONYX_DECISION_LFM2_D1_OMNI:
            token_marker_ = llama_vocab_mask(vocab_);
            if (token_marker_ == LLAMA_TOKEN_NULL) {
                throw std::runtime_error("decision model has no mask token");
            }
            n_options_max_ = 255;
            break;
        case ONYX_DECISION_H2O_LIGHTNING: {
            // the answer is read at the token of a label, its text is given to the template
            for (uint32_t i = 0; llama_model_decision_label(model, i) != nullptr; i++) {
                const std::string text = llama_model_decision_label(model, i);
                const auto toks = tokenize(text, false);
                if (toks.size() != 1 || std::find(labels_.begin(), labels_.end(), toks[0]) != labels_.end()) {
                    throw std::runtime_error(string_format("decision label \"%s\" is not a single distinct token", text.c_str()));
                }
                labels_.push_back(toks[0]);
                label_texts_.push_back(text);
            }
            if (labels_.empty()) {
                throw std::runtime_error("decision model has no labels");
            }
            // control and user-defined token texts written in the input are
            // quoted, so they stay text
            for (llama_token id = 0; id < llama_vocab_n_tokens(vocab_); id++) {
                if (llama_vocab_get_attr(vocab_, id) & (LLAMA_TOKEN_ATTR_CONTROL | LLAMA_TOKEN_ATTR_USER_DEFINED)) {
                    const std::string text = llama_vocab_get_text(vocab_, id);
                    if (!text.empty()) {
                        quoted_spellings_.push_back(text);
                    }
                }
            }
            std::stable_sort(quoted_spellings_.begin(), quoted_spellings_.end(),
                             [](const std::string & a, const std::string & b) { return a.size() > b.size(); });
            for (const auto & spelling : quoted_spellings_) {
                if (quoted_first_.find(spelling[0]) == std::string::npos) {
                    quoted_first_ += spelling[0];
                }
            }
            n_options_max_   = labels_.size();
            noul_true_first_ = true;
            break;
        }
        default:
            throw std::runtime_error("unsupported decision model type: " + type_name);
    }
    type_ = model_type;
}

bool onyx_decision::can_share_prompt() const {
    switch (type_) {
        case ONYX_DECISION_OPENJEV:
        case ONYX_DECISION_GRANITE_SWITCH:
        case ONYX_DECISION_LEV:
        case ONYX_DECISION_KEV:
        case ONYX_DECISION_NIMBLE:
        case ONYX_DECISION_PPLX_DECIDER:
        case ONYX_DECISION_VLLM_DECISION2:
        case ONYX_DECISION_LFM2_D1:
        case ONYX_DECISION_H2O_LIGHTNING:
            return true;
        default:
            return false;
    }
}

bool onyx_decision::can_use_images() const {
    switch (type_) {
        case ONYX_DECISION_OPENJEV:
        case ONYX_DECISION_CLEF:
        case ONYX_DECISION_PPLX_DECIDER:
        case ONYX_DECISION_VLLM_D3:
        case ONYX_DECISION_LFM2_D1:
        case ONYX_DECISION_LFM2_D1_OMNI:
        case ONYX_DECISION_H2O_LIGHTNING:
            return true;
        default:
            return false;
    }
}

bool onyx_decision::onyx_supports_media() const {
    // the families that read label logits at the last token of a causal
    // prompt: the engine prefills media through mtmd and reads one row.
    // clef and lfm2-d1-omni read embeddings at positions after the media,
    // and vllm-d3 needs the whole prompt in one non-causal batch; neither
    // fits the mtmd chunk-by-chunk prefill yet.
    switch (type_) {
        case ONYX_DECISION_OPENJEV:
        case ONYX_DECISION_PPLX_DECIDER:
        case ONYX_DECISION_LFM2_D1:
        case ONYX_DECISION_H2O_LIGHTNING:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// request parsing
// ---------------------------------------------------------------------------

std::vector<onyx_decision_question> onyx_decision::parse_questions(const json & body) const {
    // lfm2-d1, lfm2-d1-omni and vllm-d3 accept a null state, e.g. to ask about images only
    const bool null_state_ok = type_ == ONYX_DECISION_LFM2_D1 || type_ == ONYX_DECISION_LFM2_D1_OMNI ||
                               type_ == ONYX_DECISION_VLLM_D3;
    if (!body.contains("state") || (body.at("state").is_null() && !null_state_ok)) {
        throw std::invalid_argument("\"state\" must be provided");
    }
    if (strict_payload_ && !is_payload(body.at("state"))) {
        throw std::invalid_argument("\"state\" must be text, an object or an array");
    }
    if (!body.contains("questions") || !body.at("questions").is_object() || body.at("questions").empty()) {
        throw std::invalid_argument("\"questions\" must be a non-empty object");
    }

    const bool is_h2o = type_ == ONYX_DECISION_H2O_LIGHTNING;

    std::vector<onyx_decision_question> questions;
    for (const auto & [id, q] : body.at("questions").items()) {
        auto err = [&id = id](const std::string & msg) {
            return std::invalid_argument("questions." + id + ": " + msg);
        };
        if (!q.is_object()) {
            throw err("must be an object");
        }
        // vllm-d3 has a default question; h2o-lightning has a default
        // instruction in its template and defaults the type to choice
        const json instructions = q.contains("instructions") ? q.at("instructions") : (is_h2o ? json(std::string()) : json());
        if (instructions.is_null() && type_ != ONYX_DECISION_VLLM_D3 && !is_h2o) {
            throw err("\"instructions\" must be provided");
        }
        if (strict_payload_ && (!is_payload(instructions) || instructions == "")) {
            throw err("\"instructions\" must be non-empty text, an object or an array");
        }

        onyx_decision_question question;
        question.id           = id;
        question.instructions = instructions;

        const std::string type_name = str_field(q, "type", is_h2o ? "choice" : "");
        const json        criteria  = q.contains("criteria") ? q.at("criteria") : json();

        if (type_name == "choice") {
            question.type = ONYX_DECISION_QUESTION_CHOICE;
            if (!criteria.is_object() || criteria.empty()) {
                throw err("\"criteria\" must be a non-empty object");
            }
            for (const auto & [key, description] : criteria.items()) {
                if (strict_payload_ && (key.empty() || !(description.is_null() || is_payload(description)))) {
                    throw err("option keys must be non-empty, descriptions must be null, text, an object or an array");
                }
                question.options.push_back({ key, description });
            }
            if (strict_payload_ && question.options.size() < 2) {
                throw err("\"criteria\" must have at least 2 options");
            }
            if (choice_sorted_) {
                std::sort(question.options.begin(), question.options.end(),
                          [](const onyx_decision_option & a, const onyx_decision_option & b) { return a.key < b.key; });
            }
        } else if (type_name == "score") {
            question.type = ONYX_DECISION_QUESTION_SCORE;
            // h2o-lightning: from 1 level, the maximum is n_options_max
            if (is_h2o && (!criteria.is_array() || criteria.empty())) {
                throw err("\"criteria\" must be a non-empty array");
            }
            if (!is_h2o && (!criteria.is_array() || criteria.size() < 2 || criteria.size() > 10)) {
                throw err("\"criteria\" must be an array of 2 to 10 levels");
            }
            for (size_t i = 0; i < criteria.size(); i++) {
                if (strict_payload_ && !is_payload(criteria.at(i))) {
                    throw err("level descriptions must be text, an object or an array");
                }
                question.options.push_back({ std::to_string(i), criteria.at(i) });
            }
        } else if (type_name == "noul") {
            question.type = ONYX_DECISION_QUESTION_NOUL;
            if (!criteria.is_null() && !criteria.is_object()) {
                throw err("\"criteria\" must be an object");
            }
            if (strict_payload_ && criteria.is_object()) {
                for (const auto & [key, description] : criteria.items()) {
                    if ((key != "false" && key != "true") || !is_payload(description)) {
                        throw err("\"criteria\" may only describe \"false\" and \"true\" with text, an object or an array");
                    }
                }
            }
            for (const char * key : { "false", "true" }) {
                json description;
                if (criteria.is_object() && criteria.contains(key)) {
                    description = criteria.at(key);
                } else if (criteria.is_object() && (type_ == ONYX_DECISION_LFM2_D1_OMNI || is_h2o)) {
                    // lfm2-d1-omni and h2o-lightning also read "no" and "yes"
                    const char * alias = std::string(key) == "true" ? "yes" : "no";
                    description = criteria.contains(alias) ? criteria.at(alias) : json();
                }
                question.options.push_back({ key, description });
            }
            if (noul_true_first_ ||
                (noul_criteria_order_ && criteria.is_object() && criteria.size() == 2 && criteria.begin().key() == "true")) {
                std::swap(question.options[0], question.options[1]);
            }
        } else {
            throw err("\"type\" must be one of: choice, score, noul");
        }

        if (question.options.size() > n_options_max_) {
            throw err(string_format("too many options (%zu), this model supports at most %zu",
                                    question.options.size(), n_options_max_));
        }
        questions.push_back(std::move(question));
    }
    return questions;
}

// a data: URL (images; audio too for models that take it)
static void load_image(const json & url, std::vector<onyx_decision_media> & files,
                       const onyx_decision_decoder & decode) {
    if (!url.is_string() || !string_starts_with(url.get<std::string>(), "data:")) {
        throw std::invalid_argument("images must be data URLs (data:image/...;base64,... or data:audio/...;base64,...)");
    }
    if (files.size() >= MAX_MEDIA) {
        throw std::invalid_argument(string_format("too many images, the maximum is %zu", MAX_MEDIA));
    }
    files.push_back(decode(url.get<std::string>()));
}

// base64 data, as a data URL or not (OpenAI input_audio)
static void load_audio(const json & data, std::vector<onyx_decision_media> & files,
                       const onyx_decision_decoder & decode) {
    if (!data.is_string() || string_starts_with(data.get<std::string>(), "http") ||
        string_starts_with(data.get<std::string>(), "file://")) {
        throw std::invalid_argument("audio must be base64 data");
    }
    if (files.size() >= MAX_MEDIA) {
        throw std::invalid_argument(string_format("too many media files, the maximum is %zu", MAX_MEDIA));
    }
    files.push_back(decode(data.get<std::string>()));
}

json onyx_decision::parse_state(const json & body, std::vector<onyx_decision_media> & files,
                                const onyx_decision_decoder & decode) const {
    if (body.contains("videos") && !body.at("videos").is_null() && !body.at("videos").empty()) {
        throw std::invalid_argument("\"videos\" is not supported");
    }
    // "images" is an alias of "files"
    for (const char * key : { "files", "images" }) {
        if (!body.contains(key) || body.at(key).is_null()) {
            continue;
        }
        if (!body.at(key).is_array()) {
            throw std::invalid_argument(string_format("\"%s\" must be an array", key));
        }
        for (const auto & url : body.at(key)) {
            load_image(url, files, decode);
        }
    }

    const json & state      = body.at("state");
    const bool   is_wrapped = state.is_object() && state.contains("messages");
    const json & messages   = is_wrapped ? state.at("messages") : state;
    if (!messages.is_array()) {
        return state;
    }

    // chat messages: take the image and audio parts out of the content
    json messages_out = json::array();
    for (const auto & msg : messages) {
        if (!msg.is_object() || !msg.contains("content") || !msg.at("content").is_array()) {
            messages_out.push_back(msg);
            continue;
        }
        json content = json::array();
        for (const auto & part : msg.at("content")) {
            const std::string part_type = part.is_object() ? str_field(part, "type", "") : "";
            if (part_type == "image_url" && part.contains("image_url")) {
                const json & image_url = part.at("image_url");
                load_image(image_url.is_object() && image_url.contains("url") ? image_url.at("url") : image_url,
                           files, decode);
            } else if (part_type == "input_audio" && part.contains("input_audio")) {
                const json input_audio = part.at("input_audio").is_object() ? part.at("input_audio") : json::object();
                load_audio(input_audio.contains("data") ? input_audio.at("data") : input_audio.value("url", json()),
                           files, decode);
            } else {
                content.push_back(part);
            }
        }
        json msg_out = msg;
        msg_out["content"] = content;
        messages_out.push_back(msg_out);
    }

    if (!is_wrapped) {
        return messages_out;
    }
    json state_out = state;
    state_out["messages"] = messages_out;
    return state_out;
}

// ---------------------------------------------------------------------------
// JSON text transforms
// ---------------------------------------------------------------------------

// replace text in all strings of a JSON value
static json replace_text(const json & val, const std::string & search, const std::string & replace) {
    if (val.is_string()) {
        std::string str = val.get<std::string>();
        string_replace_all(str, search, replace);
        return json(str);
    }
    if (val.is_array()) {
        json out = json::array();
        for (const auto & item : val) {
            out.push_back(replace_text(item, search, replace));
        }
        return out;
    }
    if (val.is_object()) {
        json out = json::object();
        for (const auto & [key, item] : val.items()) {
            out[key] = replace_text(item, search, replace);
        }
        return out;
    }
    return val;
}

// sort the keys of all objects of a JSON value
static json sort_keys(const json & val) {
    if (val.is_array()) {
        json out = json::array();
        for (const auto & item : val) {
            out.push_back(sort_keys(item));
        }
        return out;
    }
    if (val.is_object()) {
        std::map<std::string, json> sorted;
        for (const auto & [key, item] : val.items()) {
            sorted[key] = sort_keys(item);
        }
        json out = json::object();
        for (const auto & [key, item] : sorted) {
            out[key] = item;
        }
        return out;
    }
    return val;
}

// kev flattens a JSON value into text, object keys kept as labels (kev/api.py: render)
static std::string kev_render(const json & val, int indent = 0) {
    const std::string pad(2 * indent, ' ');
    if (val.is_null()) {
        return "";
    }
    if (val.is_string()) {
        return val.get<std::string>();
    }
    if (val.is_boolean()) {
        return val.get<bool>() ? "True" : "False";
    }
    if (val.is_array()) {
        std::string out;
        for (const auto & item : val) {
            const std::string text = kev_render(item, indent + 1);
            out += (out.empty() ? "" : "\n") + pad + "- " + text.substr(std::min(text.size(), text.find_first_not_of(" \t\n\r")));
        }
        return out;
    }
    if (val.is_object()) {
        std::string out;
        for (const auto & [key, item] : val.items()) {
            const bool is_nested = item.is_object() || item.is_array();
            out += (out.empty() ? "" : "\n") + pad + key + (is_nested ? ":\n" : ": ") +
                   kev_render(item, is_nested ? indent + 1 : 0);
        }
        return out;
    }
    return val.dump();
}

// <\|([A-Za-z0-9_]+)\|>  ->  <¦$1¦>, without std::regex (libstdc++ recurses
// per character, a long input overflows the stack)
std::string onyx_decision_escape_special_tokens(const std::string & text) {
    const auto is_name = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    std::string out;
    size_t n_done = 0; // chars of `text` already in `out`
    for (size_t i = text.find("<|"); i != std::string::npos; i = text.find("<|", std::max(i + 1, n_done))) {
        size_t j = i + 2;
        while (j < text.size() && is_name(text[j])) {
            j++;
        }
        if (j > i + 2 && text.compare(j, 2, "|>") == 0) {
            out.append(text, n_done, i - n_done);
            out += "<\xC2\xA6";
            out.append(text, i + 2, j - i - 2);
            out += "\xC2\xA6>";
            n_done = j + 2;
        }
    }
    if (n_done == 0) {
        return text;
    }
    out.append(text, n_done, std::string::npos);
    return out;
}

// kev text input: special tokens written in the text are not parsed as such
static std::string kev_text(const json & val) {
    return onyx_decision_escape_special_tokens(kev_render(val));
}

// lfm2-d1-omni: special tokens written in the input are not parsed as such,
// in keys too (d1-omni prompt.py: escape)
static json d1omni_escape(const json & val) {
    if (val.is_string()) {
        return json(onyx_decision_escape_special_tokens(val.get<std::string>()));
    }
    if (val.is_array()) {
        json out = json::array();
        for (const auto & item : val) {
            out.push_back(d1omni_escape(item));
        }
        return out;
    }
    if (val.is_object()) {
        json out = json::object();
        for (const auto & [key, item] : val.items()) {
            out[onyx_decision_escape_special_tokens(key)] = d1omni_escape(item);
        }
        return out;
    }
    return val;
}

// ---------------------------------------------------------------------------
// h2o-lightning: texts rendered the way the reference shim renders them with
// Python (json.dumps, str.lstrip)
// ---------------------------------------------------------------------------

// one code point of UTF-8 text at s[i]; i moves past it; an invalid byte is read as is
static uint32_t utf8_next(const std::string & s, size_t & i) {
    const unsigned char c = s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 1;
    if (i + n > s.size()) {
        n = 1;
    }
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
    for (size_t k = 1; k < n; k++) {
        cp = (cp << 6) | (s[i + k] & 0x3f);
    }
    i += n;
    return cp;
}

// Python's str.isspace()
static bool py_isspace(uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0d) || (cp >= 0x1c && cp <= 0x20) || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

static std::string py_rstrip(const std::string & s) {
    size_t end = 0;
    for (size_t i = 0; i < s.size();) {
        if (!py_isspace(utf8_next(s, i))) {
            end = i;
        }
    }
    return s.substr(0, end);
}

static std::string py_lstrip(const std::string & s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t next = i;
        if (!py_isspace(utf8_next(s, next))) {
            break;
        }
        i = next;
    }
    return s.substr(i);
}

// a code point as JSON escapes, a surrogate pair above the BMP
static std::string py_escape_cp(uint32_t cp) {
    if (cp < 0x10000) {
        return string_format("\\u%04x", cp);
    }
    cp -= 0x10000;
    return string_format("\\u%04x\\u%04x", 0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
}

// repr() of a Python float: the shortest digits that read back the same,
// exponent form below 1e-4 and from 1e16
static std::string py_float(double v) {
    char buf[64];
    for (int prec = 0; prec < 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*e", prec, v);
        if (std::strtod(buf, nullptr) == v) {
            break;
        }
    }
    const std::string sci(buf); // e.g. "-1.25e+03"
    const size_t pos_e = sci.find('e');
    const int    exp   = std::atoi(sci.c_str() + pos_e + 1);
    std::string sign;
    std::string digits;
    for (size_t i = 0; i < pos_e; i++) {
        if (sci[i] == '-') {
            sign = "-";
        } else if (sci[i] != '.') {
            digits += sci[i];
        }
    }
    const int n = (int) digits.size();
    if (exp < -4 || exp >= 16) {
        return sign + digits.substr(0, 1) + (n > 1 ? "." + digits.substr(1) : "") +
               string_format("e%c%02d", exp < 0 ? '-' : '+', std::abs(exp));
    }
    if (exp >= n - 1) {
        return sign + digits + std::string(exp - (n - 1), '0') + ".0";
    }
    if (exp >= 0) {
        return sign + digits.substr(0, exp + 1) + "." + digits.substr(exp + 1);
    }
    return sign + "0." + std::string(-exp - 1, '0') + digits;
}

static void py_str(std::string & out, const std::string & s, bool ensure_ascii) {
    out += '"';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; i++; continue;
            case '\\': out += "\\\\"; i++; continue;
            case '\b': out += "\\b";  i++; continue;
            case '\f': out += "\\f";  i++; continue;
            case '\n': out += "\\n";  i++; continue;
            case '\r': out += "\\r";  i++; continue;
            case '\t': out += "\\t";  i++; continue;
            default:   break;
        }
        if (c < 0x20 || (ensure_ascii && c == 0x7f)) {
            out += string_format("\\u%04x", c);
            i++;
        } else if (c < 0x80 || !ensure_ascii) {
            out += (char) c;
            i++;
        } else {
            out += py_escape_cp(utf8_next(s, i));
        }
    }
    out += '"';
}

// Python's json.dumps(): indent < 0 is no indent, keys stay in their order
static void py_dump(std::string & out, const json & val, int indent, int level,
                    const std::string & item_sep, const std::string & key_sep, bool ensure_ascii) {
    const auto newline = [&](int lvl) {
        return indent < 0 ? std::string() : "\n" + std::string(indent * lvl, ' ');
    };
    if (val.is_null()) {
        out += "null";
    } else if (val.is_boolean()) {
        out += val.get<bool>() ? "true" : "false";
    } else if (val.is_number_float()) {
        out += py_float(val.get<double>());
    } else if (val.is_number()) {
        out += val.dump();
    } else if (val.is_string()) {
        py_str(out, val.get<std::string>(), ensure_ascii);
    } else if (val.is_array()) {
        if (val.empty()) {
            out += "[]";
            return;
        }
        out += "[" + newline(level + 1);
        for (size_t i = 0; i < val.size(); i++) {
            if (i > 0) {
                out += item_sep + newline(level + 1);
            }
            py_dump(out, val.at(i), indent, level + 1, item_sep, key_sep, ensure_ascii);
        }
        out += newline(level) + "]";
    } else if (val.is_object()) {
        if (val.empty()) {
            out += "{}";
            return;
        }
        out += "{" + newline(level + 1);
        bool first = true;
        for (const auto & [key, item] : val.items()) {
            if (!first) {
                out += item_sep + newline(level + 1);
            }
            first = false;
            py_str(out, key, ensure_ascii);
            out += key_sep;
            py_dump(out, item, indent, level + 1, item_sep, key_sep, ensure_ascii);
        }
        out += newline(level) + "}";
    }
}

static std::string py_dumps(const json & val, int indent, const std::string & item_sep,
                            const std::string & key_sep, bool ensure_ascii) {
    std::string out;
    py_dump(out, val, indent, 0, item_sep, key_sep, ensure_ascii);
    return out;
}

// a state longer than the limit keeps its head and its tail around a "..." line
std::string onyx_decision::h2o_cut_state(const std::string & text) const {
    // a byte-level BPE never has more tokens than bytes
    if (text.size() <= H2O_MAX_STATE_TOKENS) {
        return text;
    }
    const auto tokens = tokenize(text, true);
    if (tokens.size() <= H2O_MAX_STATE_TOKENS) {
        return text;
    }
    // each half is the text of its token pieces; a cut inside a character
    // gives U+FFFD, as in a JSON response of the server
    const size_t half = H2O_MAX_STATE_TOKENS / 2;
    auto text_of = [&](std::vector<llama_token>::const_iterator begin, std::vector<llama_token>::const_iterator end) {
        std::string out;
        for (auto it = begin; it != end; ++it) {
            out += onyx_token_to_piece(vocab_, *it, true);
        }
        return json::parse(json::make(out).dump_safe()).get<std::string>();
    };
    return text_of(tokens.begin(), tokens.begin() + half) + "\n...\n" + text_of(tokens.end() - half, tokens.end());
}

// special token texts written in the input are quoted with JSON escapes, so they stay text
std::string onyx_decision::h2o_quote(const std::string & text) const {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (quoted_first_.find(text[i]) == std::string::npos) {
            out += text[i++];
            continue;
        }
        bool is_quoted = false;
        for (const auto & spelling : quoted_spellings_) {
            if (text.compare(i, spelling.size(), spelling) == 0) {
                std::string quoted = spelling;
                string_replace_all(quoted, "<", "\\u003c");
                string_replace_all(quoted, ">", "\\u003e");
                out += quoted;
                i += spelling.size();
                is_quoted = true;
                break;
            }
        }
        if (!is_quoted) {
            out += text[i++];
        }
    }
    return out;
}

json onyx_decision::prepare_state(const json & state) const {
    if (type_ != ONYX_DECISION_H2O_LIGHTNING) {
        return state;
    }
    // once per request: compact JSON, the cut, the quoting
    return json(h2o_quote(h2o_cut_state(state.is_string() ? state.get<std::string>() : py_dumps(state, -1, ",", ":", false))));
}

json onyx_decision::render_h2o(const json & state, const onyx_decision_question & question) const {
    // instructions are JSON with indent 1, descriptions default JSON, the state comes from prepare_state()
    const std::string instructions = py_lstrip(question.instructions.is_string()
        ? question.instructions.get<std::string>()
        : py_dumps(question.instructions, 1, ",", ": ", true));

    json options = json::array();
    for (size_t i = 0; i < question.options.size(); i++) {
        const auto & opt = question.options[i];
        // null if not provided (the template has a default), except a score
        // level, which is shown as JSON null
        json description;
        if (question.type == ONYX_DECISION_QUESTION_SCORE && opt.description.is_null()) {
            description = "null";
        } else if (!opt.description.is_null()) {
            description = h2o_quote(opt.description.is_string() ? opt.description.get<std::string>()
                                                                 : py_dumps(opt.description, -1, ", ", ": ", false));
        }
        const std::string key = h2o_quote(opt.key);

        // the user turn is stripped like Python's str.strip(), the template
        // trims ASCII whitespace only; so the text that ends the turn is
        // stripped here, a space keeps a description that is only whitespace
        if (i + 1 == question.options.size()) {
            const bool is_key = question.type == ONYX_DECISION_QUESTION_CHOICE && description.is_null();
            if (is_key || description.is_string()) {
                const std::string text     = is_key ? key : description.get<std::string>();
                const std::string stripped = py_rstrip(text);
                if (stripped != text) {
                    description = stripped.empty() ? " " : stripped;
                }
            }
        }

        json option = json::object();
        option["label"]       = label_texts_[i];
        option["key"]         = key;
        option["description"] = description;
        options.push_back(option);
    }

    json inp = json::object();
    inp["type"]         = question_type_name(question.type);
    inp["state"]        = state;
    inp["instructions"] = h2o_quote(instructions);
    inp["options"]      = options;
    return inp;
}

// ---------------------------------------------------------------------------
// prompts
// ---------------------------------------------------------------------------

// given to the lfm2-d1-omni template: text between the pieces of the prompt,
// and at the start of the pieces cut to a token budget
static const std::string D1OMNI_MARKER        = "<<d1omni:";
static const std::string D1OMNI_SEP           = "<<d1omni:sep>>";
static const std::string D1OMNI_MARK_STATE    = "<<d1omni:state>>";
static const std::string D1OMNI_MARK_QUESTION = "<<d1omni:question>>";
static const std::string D1OMNI_MARK_OPTION   = "<<d1omni:option>>";

// max_length of the model config, the converter checks it
static const size_t D1OMNI_MAX_TOKENS = 16384;

// given to the clef template: text between the pieces of the prompt, and at
// the start of the span of a question or of an option
static const std::string CLEF_MARKER        = "<<clef:";
static const std::string CLEF_SEP           = "<<clef:sep>>";
static const std::string CLEF_MARK_QUESTION = "<<clef:question>>";
static const std::string CLEF_MARK_OPTION   = "<<clef:option>>";

size_t onyx_decision::n_variants(const onyx_decision_question & question) const {
    // lev shows a choice's options in 2 orders, to cancel its preference for the first label
    if (type_ == ONYX_DECISION_LEV && question.type == ONYX_DECISION_QUESTION_CHOICE && question.options.size() > 1) {
        return 2;
    }
    return 1;
}

size_t onyx_decision::n_outputs(const onyx_decision_question & question) const {
    if (type_ == ONYX_DECISION_LEV && question.type == ONYX_DECISION_QUESTION_NOUL) {
        return LEV_N_RATINGS;
    }
    return question.options.size();
}

// label codes follow prompt.py of the model repo
void onyx_decision::d1_labels(const onyx_decision_question & question, std::vector<std::string> & texts,
                              std::vector<std::vector<llama_token>> & groups) const {
    const size_t n_options = question.options.size();

    auto single_tokens = [&](const std::vector<std::string> & forms) {
        std::vector<llama_token> out;
        for (const auto & form : forms) {
            const auto toks = tokenize(form, false);
            if (toks.size() == 1 && std::find(out.begin(), out.end(), toks[0]) == out.end()) {
                out.push_back(toks[0]);
            }
        }
        return out;
    };

    if (question.type != ONYX_DECISION_QUESTION_CHOICE) {
        for (const auto & opt : question.options) {
            std::vector<llama_token> group;
            if (question.type == ONYX_DECISION_QUESTION_SCORE) {
                group = single_tokens({ opt.key });
            } else if (opt.key == "true") {
                group = single_tokens({ "yes", "Yes", "YES" });
            } else {
                group = single_tokens({ "no", "No", "NO" });
            }
            if (group.empty()) {
                throw std::runtime_error("decision label is not a single token: " + opt.key);
            }
            texts.push_back(opt.key);
            groups.push_back(group);
        }
        return;
    }

    bool is_letters = true;
    for (const auto & opt : question.options) {
        is_letters = is_letters && opt.key.size() == 1 && std::isalpha((unsigned char) opt.key[0]);
    }

    std::vector<std::string> codes;
    for (size_t i = 0; i < n_options; i++) {
        if (is_letters) {
            codes.push_back(question.options[i].key);
        } else if (n_options <= 26) {
            codes.push_back(std::string(1, (char) ('A' + i)));
        } else {
            codes.push_back(string_format("%02zu", i));
        }
    }

    std::vector<std::string> pool;
    for (char c = 'A'; c <= 'Z'; c++) {
        pool.push_back(std::string(1, c));
    }
    for (int i = 0; i < 100; i++) {
        pool.push_back(string_format("%02d", i));
    }
    for (char c = 'a'; c <= 'z'; c++) {
        pool.push_back(std::string(1, c));
    }
    for (int i = 0; i < 200; i++) {
        pool.push_back(string_format("#%d", i));
    }
    for (char a = 'A'; a <= 'Z'; a++) {
        for (char b = 'A'; b <= 'Z'; b++) {
            pool.push_back(std::string{ a, b });
        }
    }

    std::vector<llama_token> used;
    auto take = [&](const std::string & code) {
        const auto toks = tokenize(code, false);
        if (toks.size() != 1 || std::find(used.begin(), used.end(), toks[0]) != used.end()) {
            return false;
        }
        used.push_back(toks[0]);
        std::vector<llama_token> group = { toks[0] };
        for (const llama_token tok : single_tokens({ " " + code })) {
            if (tok != toks[0]) {
                group.push_back(tok);
            }
        }
        texts.push_back(code);
        groups.push_back(group);
        return true;
    };
    for (const auto & code : codes) {
        bool is_taken = take(code);
        for (size_t i = 0; !is_taken && i < pool.size(); i++) {
            is_taken = take(pool[i]);
        }
        if (!is_taken) {
            throw std::invalid_argument(string_format("no single-token label left for %zu options", n_options));
        }
    }
}

json onyx_decision::render_options(const onyx_decision_question & question, size_t variant) const {
    const size_t n_options = question.options.size();

    std::vector<std::string>              d1_texts;
    std::vector<std::vector<llama_token>> d1_groups;
    if (type_ == ONYX_DECISION_LFM2_D1) {
        d1_labels(question, d1_texts, d1_groups);
    }

    // the second variant shows the options in the reverse order
    json options = json::array();
    for (size_t i = 0; i < n_options; i++) {
        const auto & opt = question.options[variant == 0 ? i : n_options - 1 - i];
        json option = json::object();
        option["key"]         = opt.key;
        option["description"] = opt.description;
        if (type_ == ONYX_DECISION_KEV) {
            option["key"] = kev_text(json(opt.key));
            if (!opt.description.is_null()) {
                option["description"] = kev_text(opt.description);
            }
        }
        if (!label_texts_.empty()) {
            option["label"] = label_texts_[i];
        }
        if (!d1_texts.empty()) {
            option["label"] = d1_texts[i];
        }
        options.push_back(option);
    }
    return options;
}

std::string onyx_decision::render_template(const json & inp) const {
    return tmpl_->render(inp);
}

std::string onyx_decision::render(const json & state,
                                  const std::vector<onyx_decision_question> & questions,
                                  const onyx_decision_question & question,
                                  size_t variant, size_t n_images, bool is_audio) const {
    // the template gets raw JSON values and serializes the ones that are not
    // strings; h2o-lightning gets texts
    json inp;
    if (type_ == ONYX_DECISION_H2O_LIGHTNING) {
        inp = render_h2o(state, question);
    } else {
        inp = json::object();
        inp["id"]           = question.id;
        inp["type"]         = question_type_name(question.type);
        inp["instructions"] = question.instructions;
        inp["state"]        = state;
        inp["options"]      = render_options(question, variant);
    }

    // the nimble prompt lists all the questions of the request
    if (type_ == ONYX_DECISION_NIMBLE) {
        json all = json::array();
        for (const auto & q : questions) {
            json item = json::object();
            item["id"]           = q.id;
            item["type"]         = question_type_name(q.type);
            item["instructions"] = q.instructions;
            item["options"]      = render_options(q, 0);
            all.push_back(item);
        }
        inp["questions"] = all;
    }

    // lev was trained with sorted keys
    if (type_ == ONYX_DECISION_LEV) {
        inp = sort_keys(inp);
    }

    // the kev template only takes text
    if (type_ == ONYX_DECISION_KEV) {
        inp["state"]        = kev_text(state);
        inp["instructions"] = kev_text(question.instructions);
    }

    // the input must not contain the marker of the options
    if (!text_marker_.empty()) {
        inp = replace_text(inp, text_marker_, " ");
    }

    if (type_ == ONYX_DECISION_LFM2_D1_OMNI) {
        inp = replace_text(d1omni_escape(inp), D1OMNI_MARKER, "<<d1omni ");
        inp["audio"]         = is_audio;
        inp["sep"]           = D1OMNI_SEP;
        inp["mark_state"]    = D1OMNI_MARK_STATE;
        inp["mark_question"] = D1OMNI_MARK_QUESTION;
        inp["mark_option"]   = D1OMNI_MARK_OPTION;
    }

    // the template puts one media marker per image
    json images = json::array();
    if (n_images > 0) {
        inp = replace_text(inp, media_marker_, " ");
        for (size_t i = 0; i < n_images; i++) {
            images.push_back(media_marker_);
        }
    }
    inp["images"] = images;

    return render_template(inp);
}

onyx_decision_prompt onyx_decision::fill_prompt(const json & state,
                                                const std::vector<onyx_decision_question> & questions,
                                                const onyx_decision_question & question,
                                                size_t variant,
                                                const std::vector<onyx_decision_media> & files) const {
    // each question has its own prompt, with the spans the head reads
    if (type_ == ONYX_DECISION_VLLM_DECISION2) {
        return fill_prompt_joint(state, { question }, files);
    }

    onyx_decision_prompt out;
    if (type_ == ONYX_DECISION_LFM2_D1_OMNI) {
        fill_d1omni(state, questions, question, variant, out);
        return out;
    }

    const std::string prompt = render(state, questions, question, variant, files.size());

    switch (type_) {
        case ONYX_DECISION_OPENJEV:
        case ONYX_DECISION_GRANITE_SWITCH:
        case ONYX_DECISION_LEV:
        case ONYX_DECISION_NIMBLE:
        case ONYX_DECISION_PPLX_DECIDER:
        case ONYX_DECISION_VLLM_D3:
        case ONYX_DECISION_H2O_LIGHTNING:
            // lev reads a noul question's ratings at its first labels, not at the digits
            out.labels.assign(labels_.begin(), labels_.begin() + n_outputs(question));
            break;
        case ONYX_DECISION_LFM2_D1: {
            std::vector<std::string>              texts;
            std::vector<std::vector<llama_token>> groups;
            d1_labels(question, texts, groups);
            for (const auto & group : groups) {
                out.labels.insert(out.labels.end(), group.begin(), group.end());
                out.label_groups.push_back((int32_t) group.size());
            }
            break;
        }
        default:
            break;
    }

    if (!files.empty()) {
        // the engine tokenizes the text with the media (see onyx_supports_media())
        out.text  = prompt;
        out.media = files;
        return out;
    }

    std::vector<llama_token> tokens = tokenize(prompt, true);
    if (type_ == ONYX_DECISION_LAYA) {
        fill_laya(tokens, question, out);
    }
    if (type_ == ONYX_DECISION_KEV) {
        // an option is read at its end token, the question at the last token
        for (size_t i = 0; i < tokens.size(); i++) {
            if (tokens[i] == token_marker_) {
                out.markers.push_back((int32_t) i);
            }
        }
        if (out.markers.size() != question.options.size()) {
            throw std::runtime_error("unexpected layout of the decision prompt");
        }
        out.pointer = (int32_t) tokens.size() - 1;
    }
    out.tokens = std::move(tokens);
    return out;
}

// the prompt is: [cls] question [sep] ([marker] option)* [sep] state [sep]
// options and question are cut to max_head_tokens, as the model was trained
void onyx_decision::fill_laya(std::vector<llama_token> & tokens, const onyx_decision_question & question,
                              onyx_decision_prompt & out) const {
    const size_t n_options = question.options.size();

    std::vector<size_t> markers;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == token_marker_) {
            markers.push_back(i);
        }
    }
    const auto invalid = std::runtime_error("unexpected layout of the decision prompt");
    if (markers.size() != n_options || markers[0] < 2 || tokens[markers[0] - 1] != token_sep_ || tokens.back() != token_sep_) {
        throw invalid;
    }
    const size_t head_end = markers[0] - 1;
    const size_t opts_end = std::find(tokens.begin() + markers.back(), tokens.end(), token_sep_) - tokens.begin();
    if (opts_end + 1 >= tokens.size()) {
        throw invalid;
    }

    // marker + text of each option
    std::vector<std::vector<llama_token>> options;
    size_t n_options_tokens = 0;
    auto set_max = [&](size_t n_max) {
        n_options_tokens = 0;
        for (auto & opt : options) {
            opt.resize(std::min(opt.size(), n_max));
            n_options_tokens += opt.size();
        }
    };
    for (size_t i = 0; i < n_options; i++) {
        const size_t end = i + 1 < n_options ? markers[i + 1] : opts_end;
        options.emplace_back(tokens.begin() + markers[i], tokens.begin() + end);
    }
    set_max(max_option_tokens_ + 1);
    if (n_options_tokens + 16 > max_head_tokens_) {
        // too many or too long options, shrink them evenly
        set_max(std::max((size_t) 4, (max_head_tokens_ - std::min(max_head_tokens_, (size_t) 16)) / n_options));
    }
    const size_t n_question_max = std::max((size_t) 8, max_head_tokens_ - std::min(max_head_tokens_, n_options_tokens));

    std::vector<llama_token> res;
    res.push_back(tokens[0]);
    res.insert(res.end(), tokens.begin() + 1, tokens.begin() + std::min(head_end, 1 + n_question_max));
    res.push_back(token_sep_);
    for (const auto & opt : options) {
        out.markers.push_back((int32_t) res.size());
        res.insert(res.end(), opt.begin(), opt.end());
    }
    res.insert(res.end(), tokens.begin() + opts_end, tokens.end());
    tokens = std::move(res);

    // the output has one score per question type
    out.column = question.type;
}

// each piece is cut to its budget as the model was trained (d1-omni
// prompt.py: encode); the state gets the room that is left. Text only: with
// media, the media budget and audio layout apply (not supported here yet).
void onyx_decision::fill_d1omni(const json & state,
                                const std::vector<onyx_decision_question> & questions,
                                const onyx_decision_question & question,
                                size_t variant, onyx_decision_prompt & out) const {
    const auto invalid = std::runtime_error("unexpected layout of the decision prompt");

    const std::string        prompt = render(state, questions, question, variant, 0, false);
    std::vector<std::string> pieces = string_split(prompt, D1OMNI_SEP);
    if (pieces.empty() || !pieces[0].empty()) {
        throw invalid;
    }

    const size_t n_max = D1OMNI_MAX_TOKENS;

    // the options get max(96, min(24 n + 32, max / 2)) tokens, shared evenly
    const int64_t n_options      = (int64_t) question.options.size();
    const int64_t n_budget       = std::max<int64_t>(96, std::min<int64_t>(24 * n_options + 32, (int64_t) n_max / 2));
    const int64_t n_option_max   = std::max<int64_t>(2, (n_budget - 3 * n_options) / n_options);
    const int64_t n_question_max = std::max<int64_t>(16, n_budget);

    std::vector<llama_token> head; // before the state
    std::vector<llama_token> body; // the state
    std::vector<llama_token> tail; // after the state
    bool has_state = false;
    for (size_t i_piece = 1; i_piece < pieces.size(); i_piece++) {
        std::string piece       = pieces[i_piece];
        int64_t     n_piece_max = -1;
        bool        is_state    = false;
        if (string_starts_with(piece, D1OMNI_MARK_STATE)) {
            piece    = piece.substr(D1OMNI_MARK_STATE.size());
            is_state = true;
        } else if (string_starts_with(piece, D1OMNI_MARK_QUESTION)) {
            piece       = piece.substr(D1OMNI_MARK_QUESTION.size());
            n_piece_max = n_question_max;
        } else if (string_starts_with(piece, D1OMNI_MARK_OPTION)) {
            piece       = piece.substr(D1OMNI_MARK_OPTION.size());
            n_piece_max = n_option_max;
        }

        std::vector<llama_token> tokens = tokenize(piece, true);
        if (n_piece_max >= 0 && (int64_t) tokens.size() > n_piece_max) {
            tokens.resize((size_t) n_piece_max);
        }

        if (is_state) {
            if (has_state) {
                throw invalid;
            }
            body      = std::move(tokens);
            has_state = true;
        } else {
            auto & dst = has_state ? tail : head;
            dst.insert(dst.end(), tokens.begin(), tokens.end());
        }
    }
    if (!has_state) {
        throw invalid;
    }

    const size_t n_room = n_max - std::min(n_max, head.size() + tail.size());
    body.resize(std::min(body.size(), n_room));

    std::vector<llama_token> tokens = std::move(head);
    tokens.insert(tokens.end(), body.begin(), body.end());
    tokens.insert(tokens.end(), tail.begin(), tail.end());
    tokens.resize(std::min(tokens.size(), n_max));

    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == token_marker_) {
            out.markers.push_back((int32_t) i);
        }
    }
    if ((int64_t) out.markers.size() != n_options) {
        throw std::invalid_argument("the options do not fit in the context");
    }

    // the output has one score per question type
    out.column = question.type;
    out.tokens = std::move(tokens);
}

onyx_decision_prompt onyx_decision::fill_prompt_joint(const json & state,
                                                      const std::vector<onyx_decision_question> & questions,
                                                      const std::vector<onyx_decision_media> & files) const {
    if (!files.empty()) {
        throw std::invalid_argument("image input is not supported for this decision model");
    }

    json inp_questions = json::array();
    for (const auto & question : questions) {
        json options = json::array();
        for (const auto & opt : question.options) {
            json option = json::object();
            option["key"]         = opt.key;
            option["description"] = opt.description;
            options.push_back(option);
        }
        json item = json::object();
        item["id"]           = question.id;
        item["type"]         = question_type_name(question.type);
        item["instructions"] = question.instructions;
        item["options"]      = options;
        inp_questions.push_back(item);
    }

    // raw JSON values with sorted keys, and no marker in the input
    json inp = json::object();
    inp["state"]     = state;
    inp["questions"] = inp_questions;
    inp = replace_text(sort_keys(inp), CLEF_MARKER, "<<clef ");

    inp["images"]        = json::array();
    inp["sep"]           = CLEF_SEP;
    inp["mark_question"] = CLEF_MARK_QUESTION;
    inp["mark_option"]   = CLEF_MARK_OPTION;

    const std::string prompt = render_template(inp);
    const auto invalid = std::runtime_error("unexpected layout of the decision prompt");

    // the model was trained with the pieces tokenized one by one
    onyx_decision_prompt out;
    size_t i_question = 0;
    for (std::string piece : string_split(prompt, CLEF_SEP)) {
        int32_t order = LLAMA_DECISION_ORDER_NONE;
        if (string_starts_with(piece, CLEF_MARK_QUESTION)) {
            piece = piece.substr(CLEF_MARK_QUESTION.size());
            if (i_question >= questions.size()) {
                throw invalid;
            }
            switch (questions[i_question++].type) {
                case ONYX_DECISION_QUESTION_NOUL:   order = LLAMA_DECISION_ORDER_QUESTION_NOUL;   break;
                case ONYX_DECISION_QUESTION_CHOICE: order = LLAMA_DECISION_ORDER_QUESTION_CHOICE; break;
                case ONYX_DECISION_QUESTION_SCORE:  order = LLAMA_DECISION_ORDER_QUESTION_SCORE;  break;
            }
        } else if (string_starts_with(piece, CLEF_MARK_OPTION)) {
            piece = piece.substr(CLEF_MARK_OPTION.size());
            order = LLAMA_DECISION_ORDER_OPTION;
            out.n_scores++;
        }

        const auto piece_tokens = tokenize(piece, true);
        if (order != LLAMA_DECISION_ORDER_NONE && piece_tokens.empty()) {
            throw std::invalid_argument("the instructions and the options of a question must not be empty");
        }
        out.tokens.insert(out.tokens.end(), piece_tokens.begin(), piece_tokens.end());
        out.order.resize(out.tokens.size(), order);
    }

    size_t n_options = 0;
    for (const auto & question : questions) {
        n_options += question.options.size();
    }
    if (i_question != questions.size() || (size_t) out.n_scores != n_options) {
        throw invalid;
    }
    return out;
}

// ---------------------------------------------------------------------------
// answers
// ---------------------------------------------------------------------------

float onyx_decision::get_temperature(const onyx_decision_question & question) const {
    const size_t      n         = question.options.size();
    const std::string type_name = question_type_name(question.type);

    // the temperature can depend on the number of options, in the buckets used to fit it
    std::string bucket;
    if (type_ == ONYX_DECISION_LEV) {
        bucket = n <= 8 ? "small" : n <= 26 ? "mid" : "large";
    } else {
        bucket = n <= 2 ? "2" : n <= 5 ? "3_5" : n <= 10 ? "6_10" : "11";
    }

    for (const auto & name : { type_name + "." + bucket, type_name }) {
        const auto it = temperatures_.find(name);
        if (it != temperatures_.end()) {
            return it->second;
        }
    }
    return 1.0f;
}

// the confidence formulas are the ones TypeSafe publishes

static double confidence_choice(const std::vector<double> & probs) {
    if (probs.size() < 2) {
        return 1.0;
    }
    const double uniform = 1.0 / probs.size();
    const double p_max   = *std::max_element(probs.begin(), probs.end());
    return std::max(0.0, (p_max - uniform) / (1.0 - uniform));
}

static double confidence_score(const std::vector<double> & probs) {
    if (probs.size() < 2) {
        return 1.0;
    }
    const size_t n    = probs.size();
    const size_t mode = std::max_element(probs.begin(), probs.end()) - probs.begin();

    // mean distance to the mode, relative to that of a uniform distribution around its center
    double dist         = 0.0;
    double dist_uniform = 0.0;
    for (size_t i = 0; i < n; i++) {
        dist         += probs[i] * std::fabs((double) i - (double) mode);
        dist_uniform += std::fabs((double) i - (n - 1) / 2.0) / n;
    }
    return std::max(0.0, 1.0 - dist / dist_uniform);
}

// vllm-decision2, vllm-d3: normalized entropy, as in the reference implementation
static double confidence_entropy(const std::vector<double> & probs) {
    if (probs.size() < 2) {
        return 1.0;
    }
    double entropy = 0.0;
    for (const double p : probs) {
        if (p > 0.0) {
            entropy -= p * std::log(p);
        }
    }
    return std::max(0.0, std::min(1.0, 1.0 - entropy / std::log((double) probs.size())));
}

double onyx_decision::get_confidence(onyx_decision_question_type question_type, const std::vector<double> & probs) const {
    if (type_ == ONYX_DECISION_VLLM_DECISION2 || type_ == ONYX_DECISION_VLLM_D3) {
        return confidence_entropy(probs);
    }
    switch (question_type) {
        case ONYX_DECISION_QUESTION_CHOICE: return confidence_choice(probs);
        case ONYX_DECISION_QUESTION_SCORE:  return confidence_score(probs);
        case ONYX_DECISION_QUESTION_NOUL:   break;
    }
    throw std::runtime_error("no confidence for this question type");
}

json onyx_decision::format_answer(const onyx_decision_question & question,
                                  const std::vector<std::vector<float>> & scores,
                                  bool has_media) const {
    const size_t n = n_outputs(question);
    if (scores.size() != n_variants(question)) {
        throw std::runtime_error("decision result does not match the number of variants");
    }

    // softmax over each variant's outputs, then the average of the variants;
    // lfm2-d1-omni media answers are not calibrated
    float temperature = has_media && type_ == ONYX_DECISION_LFM2_D1_OMNI ? 1.0f : get_temperature(question);
    // a model can have its own temperature for requests with media
    const auto it_image = temperatures_.find("image");
    if (has_media && it_image != temperatures_.end()) {
        temperature = it_image->second;
    }

    std::vector<double> probs(n, 0.0);
    for (size_t v = 0; v < scores.size(); v++) {
        const auto & s = scores[v];
        if (s.size() != n) {
            throw std::runtime_error("decision result does not match the number of options");
        }
        // a joint head returns NaN when it could not use the decision order
        if (std::any_of(s.begin(), s.end(), [](float x) { return std::isnan(x); })) {
            throw std::runtime_error("the model could not evaluate the decision");
        }
        std::vector<double> logits(s.begin(), s.end());
        if (question.type == ONYX_DECISION_QUESTION_SCORE && score_bias_.count(n) > 0) {
            for (size_t i = 0; i < n; i++) {
                logits[i] += score_bias_.at(n)[i];
            }
        }
        const double logit_max = *std::max_element(logits.begin(), logits.end());
        std::vector<double> p(n);
        double sum = 0.0;
        for (size_t i = 0; i < n; i++) {
            p[i] = std::exp((logits[i] - logit_max) / temperature);
            sum += p[i];
        }
        for (size_t i = 0; i < n; i++) {
            // the second variant is in the reverse order
            probs[v == 0 ? i : n - 1 - i] += p[i] / sum / scores.size();
        }
    }

    json answer = json::object();
    answer["type"] = question_type_name(question.type);

    if (question.type == ONYX_DECISION_QUESTION_NOUL) {
        if (type_ == ONYX_DECISION_LEV) {
            double expected = 0.0;
            for (size_t i = 0; i < n; i++) {
                expected += probs[i] * i / (n - 1);
            }
            answer["noul"] = expected;
            return answer;
        }
        for (size_t i = 0; i < n; i++) {
            if (question.options[i].key == "true") {
                answer["noul"] = probs[i];
            }
        }
        return answer;
    }

    json probabilities = json::object();
    for (size_t i = 0; i < n; i++) {
        probabilities[question.options[i].key] = probs[i];
    }

    if (question.type == ONYX_DECISION_QUESTION_CHOICE) {
        const size_t best = std::max_element(probs.begin(), probs.end()) - probs.begin();
        answer["choice"]        = question.options[best].key;
        answer["probabilities"] = probabilities;
        answer["confidence"]    = get_confidence(question.type, probs);
    } else {
        double expected = 0.0;
        json   legend   = json::object();
        for (size_t i = 0; i < n; i++) {
            expected += i * probs[i];
            const json & description = question.options[i].description;
            legend[question.options[i].key] = canonical_legend_ && !description.is_string()
                ? json(sort_keys(description).dump()) : description;
        }
        answer["score"]         = expected;
        answer["legend"]        = legend;
        answer["probabilities"] = probabilities;
        answer["confidence"]    = get_confidence(question.type, probs);
    }
    return answer;
}
