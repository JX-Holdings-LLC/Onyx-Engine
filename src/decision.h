// onyx_decision: typed decision ("System One") models behind POST /v1/systemone.
//
// A decision model answers typed questions (choice / score / noul) about a
// `state` from one prefill: no token is generated. Which output is read, and
// how the prompt is laid out, depends on the model family, named by the GGUF
// key "<arch>.decision.type". The families and their prompt rules follow
// llama-server's server-decision.cpp at the vendored pin (ggml-org/llama.cpp
// #29818 and the model PRs after it); this is onyx-engine's own port of that
// handler, on llama.h plus common's chat-template and JSON helpers. The model
// graphs themselves (decision heads, label tokens) live in libllama.
//
// This file only builds prompts and turns raw outputs into answers. Running
// the prompts is onyx_engine::decide() (engine.cpp).
#pragma once

#include "json.h"
#include "llama.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct common_chat_template;

// one decoded media file, same as onyx_media_buffer in engine.h
using onyx_decision_media = std::vector<unsigned char>;

// data: URL or bare base64 -> file bytes; throws on bad data
using onyx_decision_decoder = std::function<onyx_decision_media(const std::string &)>;

enum onyx_decision_type {
    ONYX_DECISION_NONE,           // not a decision model
    ONYX_DECISION_OPENJEV,        // logits of one label token per option, at the last prompt token
    ONYX_DECISION_LEV,            // same as openjev, noul is read from a rating scale
    ONYX_DECISION_KEV,            // dot product of the hidden states of the last token and of one end token per option
    ONYX_DECISION_NIMBLE,         // same as openjev, the prompt lists all the questions of the request
    ONYX_DECISION_LAYA,           // score of one marker token per option, read from the embeddings output
    ONYX_DECISION_CLEF,           // all questions in one prompt, score of option i at embeddings row i
    ONYX_DECISION_PPLX_DECIDER,   // same as openjev, label codes of 1 or 2 letters
    ONYX_DECISION_VLLM_DECISION2, // score of each option from the embeddings output, options marked in the batch
    ONYX_DECISION_VLLM_D3,        // same as pplx-decider, non-causal attention
    ONYX_DECISION_LFM2_D1,        // same as openjev, the labels depend on the question type
    ONYX_DECISION_LFM2_D1_OMNI,   // same as laya, other prompt layout
    ONYX_DECISION_GRANITE_SWITCH, // same as openjev, run by the decision aLoRA of granite switch
    ONYX_DECISION_H2O_LIGHTNING,  // same as openjev, the labels are read after an answer prefill
    ONYX_DECISION_UNKNOWN,        // has "<arch>.decision.type", but not one onyx-engine knows
};

// Reads "<arch>.decision.type" from the model metadata.
onyx_decision_type onyx_decision_type_of(const llama_model * model);
const char *       onyx_decision_type_name(onyx_decision_type type);

// True for the families whose answers come from the embeddings output: the
// context has to be created with embeddings on and no pooling.
bool onyx_decision_reads_embeddings(onyx_decision_type type);

enum onyx_decision_question_type {
    ONYX_DECISION_QUESTION_CHOICE,
    ONYX_DECISION_QUESTION_SCORE,
    ONYX_DECISION_QUESTION_NOUL,
};

struct onyx_decision_option {
    std::string key;
    common_json description; // null if not provided
};

struct onyx_decision_question {
    std::string                        id;
    onyx_decision_question_type        type = ONYX_DECISION_QUESTION_CHOICE;
    common_json                        instructions;
    std::vector<onyx_decision_option>  options; // in the order of the model outputs
};

// One prompt to evaluate and where its outputs are read. Exactly one way of
// reading applies: `labels` (logits at the last prompt token), `markers`
// (embeddings rows at prompt positions) or `n_scores` (a joint head's first
// embeddings rows, with `order` marking the spans it reads).
struct onyx_decision_prompt {
    std::vector<llama_token> tokens;      // text-only prompt

    // with media: the text (one media marker per buffer) and the buffers;
    // `tokens` is then empty and only `labels` reading is used
    std::string                      text;
    std::vector<onyx_decision_media> media;

    std::vector<llama_token> labels;       // logits of these tokens, at the last prompt token
    std::vector<int32_t>     label_groups; // if set, labels per output; the output is their max
    std::vector<int32_t>     markers;      // embeddings[column] at these prompt positions
    int32_t                  column  = 0;
    int32_t                  pointer = -1; // kev: output is scaled dot(q[pointer], k[marker])

    std::vector<int32_t> order;            // joint head: llama_decision_order per prompt token
    int32_t              n_scores = 0;     // joint head: scores are the first n_scores rows

    bool reads_embeddings() const { return !markers.empty() || !order.empty(); }

    // first prompt position whose output is read, -1 if only the last one is
    int32_t pos_first() const;
};

class onyx_decision {
public:
    onyx_decision();
    ~onyx_decision();

    // Reads the "<arch>.decision.*" metadata. Leaves type() at NONE for a
    // model that is not a decision model; throws std::runtime_error for one
    // that is, but cannot be served.
    void init(const llama_model * model, const std::string & media_marker);

    onyx_decision_type type() const { return type_; }

    // the questions of a request start with the same tokens, and the model
    // can continue from them (causal, per-question prompts)
    bool can_share_prompt() const;

    // all the questions of a request go into one prompt (clef)
    bool is_joint() const { return type_ == ONYX_DECISION_CLEF; }

    // the prompt has a place for images
    bool can_use_images() const;

    // images work for this family in onyx-engine (see fill_prompt())
    bool onyx_supports_media() const;

    // All of these throw std::invalid_argument on a bad request.
    std::vector<onyx_decision_question> parse_questions(const common_json & body) const;

    // The state without its media parts; media go to `files`, in order.
    common_json parse_state(const common_json & body, std::vector<onyx_decision_media> & files,
                            const onyx_decision_decoder & decode) const;

    // The state as the model reads it; done once per request.
    common_json prepare_state(const common_json & state) const;

    // prompts evaluated for one question, each shows the options in another order
    size_t n_variants(const onyx_decision_question & question) const;

    onyx_decision_prompt fill_prompt(const common_json & state,
                                     const std::vector<onyx_decision_question> & questions,
                                     const onyx_decision_question & question,
                                     size_t variant,
                                     const std::vector<onyx_decision_media> & files) const;

    // clef: one prompt for all the questions; the scores of all their options, in order
    onyx_decision_prompt fill_prompt_joint(const common_json & state,
                                           const std::vector<onyx_decision_question> & questions,
                                           const std::vector<onyx_decision_media> & files) const;

    // scores: the raw model outputs of each variant
    common_json format_answer(const onyx_decision_question & question,
                              const std::vector<std::vector<float>> & scores,
                              bool has_media) const;

private:
    onyx_decision_type type_  = ONYX_DECISION_NONE;
    const llama_vocab * vocab_ = nullptr;
    std::string         media_marker_;
    std::unique_ptr<common_chat_template> tmpl_; // the "systemone" template

    std::map<std::string, float>          temperatures_; // "<type>" or "<type>.<bucket>"
    std::map<size_t, std::vector<float>>  score_bias_;   // by number of score levels
    size_t n_options_max_       = 0;
    bool   noul_true_first_     = false; // noul options are [true, false]
    bool   noul_criteria_order_ = false; // noul options follow the criteria keys, if given
    bool   choice_sorted_       = false; // choice options are sorted by key
    bool   strict_payload_      = false; // state, instructions, descriptions must be text, object or array
    bool   canonical_legend_    = false; // non-string score descriptions come back as canonical JSON text

    // OPENJEV, LEV, NIMBLE, PPLX_DECIDER, VLLM_D3, GRANITE_SWITCH, H2O_LIGHTNING
    std::vector<llama_token> labels_;
    std::vector<std::string> label_texts_; // only if the template is given each option's label

    // LAYA, KEV, LFM2_D1_OMNI
    llama_token token_marker_      = LLAMA_TOKEN_NULL;
    llama_token token_sep_         = LLAMA_TOKEN_NULL;
    std::string text_marker_;
    size_t      max_head_tokens_   = 0;
    size_t      max_option_tokens_ = 48;

    // H2O_LIGHTNING
    std::vector<std::string> quoted_spellings_; // control/user-defined token texts, longest first
    std::string              quoted_first_;     // their first bytes

    std::vector<llama_token> tokenize(const std::string & text, bool parse_special) const;
    std::string render_template(const common_json & inp) const;
    std::string render(const common_json & state,
                       const std::vector<onyx_decision_question> & questions,
                       const onyx_decision_question & question,
                       size_t variant, size_t n_images, bool is_audio = false) const;
    common_json render_options(const onyx_decision_question & question, size_t variant) const;
    size_t      n_outputs(const onyx_decision_question & question) const;
    void        d1_labels(const onyx_decision_question & question,
                          std::vector<std::string> & texts,
                          std::vector<std::vector<llama_token>> & groups) const;
    common_json render_h2o(const common_json & state, const onyx_decision_question & question) const;
    std::string h2o_cut_state(const std::string & text) const;
    std::string h2o_quote(const std::string & text) const;
    void        fill_laya(std::vector<llama_token> & tokens, const onyx_decision_question & question,
                          onyx_decision_prompt & out) const;
    void        fill_d1omni(const common_json & state,
                            const std::vector<onyx_decision_question> & questions,
                            const onyx_decision_question & question,
                            size_t variant, onyx_decision_prompt & out) const;

    float  get_temperature(const onyx_decision_question & question) const;
    double get_confidence(onyx_decision_question_type question_type, const std::vector<double> & probs) const;
};

// Writes "<|name|>" with U+00A6 in place of "|", so text from the request is
// not parsed as a special token.
std::string onyx_decision_escape_special_tokens(const std::string & text);
