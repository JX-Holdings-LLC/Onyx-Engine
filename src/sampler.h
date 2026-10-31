// onyx_sampler: the per-request token sampler, on llama.h's sampler API.
//
// Stage 1 of the common/ plan (reports/Replacing llama cpp common code.md):
// this replaces common_sampler. It builds the same chain upstream's
// common_sampler_init builds for the parameters onyx-engine accepts, in
// upstream's default order (logit bias for the vocab's suppress tokens,
// penalties, top-k, top-p, min-p, temperature, then dist or greedy), skipping
// the samplers that are identity at onyx-engine's settings (DRY, top-n-sigma,
// typical, XTC, and penalties left at their defaults), so a given seed gives
// the same tokens as before.
//
// The grammar sampler is kept outside the chain and applied by rejection,
// exactly as upstream does: sample from the chain, check only the chosen token
// against the grammar, and mask the full vocabulary (then sample again) only
// when that token is refused. All slots sample on the one engine thread, so a
// full-vocabulary mask on every token would stall every slot.
#pragma once

#include "llama.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Where a grammar came from, which decides whether the generation prompt is
// fed to it (upstream's common_grammar_type).
enum onyx_grammar_kind {
    ONYX_GRAMMAR_NONE,
    ONYX_GRAMMAR_USER,          // the request's own GBNF ("grammar"); never prefilled
    ONYX_GRAMMAR_OUTPUT_FORMAT, // built from json_schema / response_format
    ONYX_GRAMMAR_TOOL_CALLS,    // built by the chat layer for tool calls
};

// A lazy grammar starts constraining once one of these appears in the output.
enum onyx_trigger_kind {
    ONYX_TRIGGER_WORD,         // literal text, anywhere
    ONYX_TRIGGER_PATTERN,      // regex, matched anywhere
    ONYX_TRIGGER_PATTERN_FULL, // regex the whole output so far must match
    ONYX_TRIGGER_TOKEN,        // one token id
};

struct onyx_grammar_trigger {
    onyx_trigger_kind kind  = ONYX_TRIGGER_WORD;
    std::string       value;                    // text or regex (TOKEN: the token's text)
    llama_token       token = LLAMA_TOKEN_NULL; // TOKEN only
};

// Every sampling parameter onyx-engine accepts. The defaults are upstream's
// (common_params_sampling) so an unset field samples as it always has.
struct onyx_sampling_params {
    uint32_t seed            = LLAMA_DEFAULT_SEED;
    float    temp            = 0.80f;  // 0 = greedy
    int32_t  top_k           = 40;     // <= 0: the whole vocabulary
    float    top_p           = 0.95f;  // 1 = off
    float    min_p           = 0.05f;  // 0 = off
    int32_t  penalty_last_n  = 64;     // 0 = no penalties
    float    penalty_repeat  = 1.00f;  // 1 = off
    float    penalty_freq    = 0.00f;  // 0 = off
    float    penalty_present = 0.00f;  // 0 = off

    onyx_grammar_kind                 grammar_kind = ONYX_GRAMMAR_NONE;
    std::string                       grammar;            // GBNF; empty = none
    bool                              grammar_lazy = false;
    std::vector<onyx_grammar_trigger> grammar_triggers;   // lazy grammars only
    std::set<llama_token>             preserved_tokens;   // control tokens rendered as text
    std::string                       generation_prompt;  // what the template put after the last turn
};

// The regex a trigger becomes for llama_sampler_init_grammar_lazy_patterns
// (WORD is escaped, PATTERN_FULL anchored with ^...$). TOKEN has no pattern:
// returns an empty string.
std::string onyx_trigger_pattern(const onyx_grammar_trigger & trigger);

class onyx_sampler {
public:
    // Throws std::invalid_argument for an out-of-range penalty and
    // std::runtime_error for a grammar that does not parse.
    onyx_sampler(const llama_model * model, const onyx_sampling_params & params);
    ~onyx_sampler();

    onyx_sampler(const onyx_sampler &) = delete;
    onyx_sampler & operator=(const onyx_sampler &) = delete;

    // Samples one token from logits row `idx` of the last decode.
    llama_token sample(llama_context * ctx, int32_t idx);

    // Records a token in the sampler state. `is_generated` is false for a
    // token the engine forced (reasoning budget): the chain (penalties) sees
    // it, the grammar does not.
    void accept(llama_token token, bool is_generated);

private:
    llama_sampler * chain_ = nullptr;
    llama_sampler * grmr_  = nullptr;   // null without a grammar
    int32_t         n_vocab_ = 0;

    std::vector<llama_token_data> cur_;
    llama_token_data_array        cur_p_ {};

    void set_logits(llama_context * ctx, int32_t idx);
};
