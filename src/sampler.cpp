#include "sampler.h"

#include "llama_util.h"

#include <cctype>
#include <cmath>
#include <stdexcept>

std::string onyx_trigger_pattern(const onyx_grammar_trigger & trigger) {
    switch (trigger.kind) {
        case ONYX_TRIGGER_WORD: {
            // the characters std::regex treats as special (upstream's regex_escape)
            static const std::string special = ".^$|()*+?[]{}\\";
            std::string out;
            out.reserve(trigger.value.size() * 2);
            for (const char c : trigger.value) {
                if (special.find(c) != std::string::npos) {
                    out += '\\';
                }
                out += c;
            }
            return out;
        }
        case ONYX_TRIGGER_PATTERN:
            return trigger.value;
        case ONYX_TRIGGER_PATTERN_FULL: {
            const std::string & p = trigger.value;
            if (p.empty()) {
                return "^$";
            }
            return (p.front() != '^' ? "^" : "") + p + (p.back() != '$' ? "$" : "");
        }
        case ONYX_TRIGGER_TOKEN:
            break;
    }
    return std::string();
}

// Builds the grammar sampler (null when there is no grammar); throws when the
// grammar does not parse.
static llama_sampler * init_grammar(const llama_vocab * vocab, const onyx_sampling_params & params) {
    if (params.grammar.empty()) {
        return nullptr;
    }
    if (params.grammar.compare(0, 11, "%llguidance") == 0) {
        throw std::runtime_error("failed to parse grammar: llguidance is not enabled");
    }

    llama_sampler * grmr = nullptr;
    if (params.grammar_lazy) {
        std::vector<std::string> patterns;
        std::vector<llama_token> tokens;
        for (const auto & trigger : params.grammar_triggers) {
            if (trigger.kind == ONYX_TRIGGER_TOKEN) {
                tokens.push_back(trigger.token);
            } else {
                patterns.push_back(onyx_trigger_pattern(trigger));
            }
        }
        std::vector<const char *> patterns_c;
        patterns_c.reserve(patterns.size());
        for (const auto & p : patterns) {
            patterns_c.push_back(p.c_str());
        }
        grmr = llama_sampler_init_grammar_lazy_patterns(vocab, params.grammar.c_str(), "root",
                                                        patterns_c.data(), patterns_c.size(),
                                                        tokens.data(), tokens.size());
    } else {
        grmr = llama_sampler_init_grammar(vocab, params.grammar.c_str(), "root");
    }
    if (!grmr) {
        throw std::runtime_error("failed to parse grammar");
    }

    // A schema or tool-call grammar starts where the template's generation
    // prompt ends, so the grammar is advanced past those tokens first. The
    // request's own grammar describes only what is generated: never prefilled.
    const bool prefill = !params.grammar_lazy && !params.generation_prompt.empty() &&
        (params.grammar_kind == ONYX_GRAMMAR_OUTPUT_FORMAT || params.grammar_kind == ONYX_GRAMMAR_TOOL_CALLS);
    if (prefill) {
        const auto tokens = onyx_tokenize(vocab, params.generation_prompt, /* add_special */ false, /* parse_special */ true);
        try {
            for (size_t i = 0; i < tokens.size(); i++) {
                const std::string piece = onyx_token_to_piece(vocab, tokens[i], true);
                // some tokenizers put a space before the first special token
                if (i == 0 && !piece.empty() && std::isspace((unsigned char) piece[0]) &&
                    !std::isspace((unsigned char) params.generation_prompt[0])) {
                    continue;
                }
                llama_sampler_accept(grmr, tokens[i]);
            }
        } catch (...) {
            llama_sampler_free(grmr);
            throw;
        }
    }
    return grmr;
}

onyx_sampler::onyx_sampler(const llama_model * model, const onyx_sampling_params & params) {
    if (!std::isfinite(params.penalty_repeat) || params.penalty_repeat <= 0.0f ||
        !std::isfinite(1.0f / params.penalty_repeat)) {
        throw std::invalid_argument("penalty_repeat must be finite and greater than 0");
    }
    if (!std::isfinite(params.penalty_freq)) {
        throw std::invalid_argument("penalty_freq must be finite");
    }
    if (!std::isfinite(params.penalty_present)) {
        throw std::invalid_argument("penalty_present must be finite");
    }

    const llama_vocab * vocab = llama_model_get_vocab(model);
    n_vocab_ = llama_vocab_n_tokens(vocab);

    grmr_ = init_grammar(vocab, params);

    llama_sampler_chain_params cparams = llama_sampler_chain_default_params();
    cparams.no_perf = true;
    chain_ = llama_sampler_chain_init(cparams);

    // the tokens the model's metadata says must never be produced
    int32_t n_suppress = 0;
    const llama_token * suppress = llama_vocab_get_suppress_tokens(vocab, &n_suppress);
    if (n_suppress > 0) {
        std::vector<llama_logit_bias> bias;
        for (int32_t i = 0; i < n_suppress; i++) {
            bias.push_back({ suppress[i], -INFINITY });
        }
        llama_sampler_chain_add(chain_, llama_sampler_init_logit_bias(n_vocab_, (int32_t) bias.size(), bias.data()));
    }

    // upstream's default order; libllama hands back an empty sampler for
    // penalties left at their defaults
    llama_sampler_chain_add(chain_, llama_sampler_init_penalties(n_vocab_, params.penalty_last_n, params.penalty_repeat,
                                                                 params.penalty_freq, params.penalty_present));
    llama_sampler_chain_add(chain_, llama_sampler_init_top_k(params.top_k));
    llama_sampler_chain_add(chain_, llama_sampler_init_top_p(params.top_p, 0));
    llama_sampler_chain_add(chain_, llama_sampler_init_min_p(params.min_p, 0));
    llama_sampler_chain_add(chain_, llama_sampler_init_temp_ext(params.temp, 0.0f, 1.0f));
    // temperature 0 picks the top token; anything else draws from the seed
    // (a negative temperature leaves one candidate, so the draw is fixed too)
    llama_sampler_chain_add(chain_, params.temp == 0.0f ? llama_sampler_init_greedy()
                                                        : llama_sampler_init_dist(params.seed));
}

onyx_sampler::~onyx_sampler() {
    llama_sampler_free(grmr_);
    llama_sampler_free(chain_);
}

void onyx_sampler::set_logits(llama_context * ctx, int32_t idx) {
    const float * logits = llama_get_logits_ith(ctx, idx);
    if (!logits) {
        throw std::runtime_error("no logits for the sampled row");
    }
    cur_.resize((size_t) n_vocab_);
    for (llama_token t = 0; t < n_vocab_; t++) {
        cur_[(size_t) t] = llama_token_data{ t, logits[t], 0.0f };
    }
    cur_p_ = { cur_.data(), cur_.size(), -1, false };
}

llama_token onyx_sampler::sample(llama_context * ctx, int32_t idx) {
    llama_synchronize(ctx);

    set_logits(ctx, idx);
    llama_sampler_apply(chain_, &cur_p_);
    if (cur_p_.selected < 0) {
        throw std::runtime_error("the sampler chain selected no token");
    }
    const llama_token id = cur_p_.data[cur_p_.selected].id;
    if (!grmr_) {
        return id;
    }

    // rejection: check only the chosen token against the grammar
    llama_token_data       one   = { id, 1.0f, 0.0f };
    llama_token_data_array one_p = { &one, 1, -1, false };
    llama_sampler_apply(grmr_, &one_p);
    if (one_p.data[0].logit != -INFINITY) {
        return id;
    }

    // refused: mask the whole vocabulary with the grammar, then sample again
    set_logits(ctx, idx);
    llama_sampler_apply(grmr_, &cur_p_);
    llama_sampler_apply(chain_, &cur_p_);
    if (cur_p_.selected < 0) {
        throw std::runtime_error("the sampler chain selected no token");
    }
    return cur_p_.data[cur_p_.selected].id;
}

void onyx_sampler::accept(llama_token token, bool is_generated) {
    if (grmr_ && is_generated) {
        llama_sampler_accept(grmr_, token);
    }
    llama_sampler_accept(chain_, token);
}
