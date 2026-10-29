// Onyx-owned helpers over the public llama.h API: tokenize, detokenize,
// token-to-piece, llama_batch filling and log filtering.
//
// Stage 1 of the plan to stop linking llama.cpp's common/ library
// (reports/Replacing llama cpp common code.md): these replace
// common_tokenize, common_detokenize, common_token_to_piece and
// common_batch_add/clear, which upstream reshapes from one pin to the next.
// They depend on llama.h and ggml only, never on common/.
#pragma once

#include "llama.h"

#include <string>
#include <vector>

// Text to tokens. Throws std::runtime_error when the result would not fit an
// int32_t count (llama_tokenize's own overflow signal).
std::vector<llama_token> onyx_tokenize(const llama_vocab * vocab, const std::string & text,
                                       bool add_special, bool parse_special);

// One token's text. `special` renders control tokens instead of dropping them.
std::string onyx_token_to_piece(const llama_vocab * vocab, llama_token token, bool special);

// Tokens to text, the way llama_detokenize joins them (no leading-space strip).
std::string onyx_detokenize(const llama_vocab * vocab, const std::vector<llama_token> & tokens, bool special);

// llama_batch fill helpers (upstream removed common_batch_add/clear when
// common moved to llama_batch_ext; onyx-engine keeps the plain llama_batch).
void onyx_batch_clear(llama_batch & batch);
void onyx_batch_add(llama_batch & batch, llama_token id, llama_pos pos,
                    const std::vector<llama_seq_id> & seq_ids, bool logits);

// Routes llama.cpp's, ggml's and mtmd's logging to stderr at warning level
// and above, so a supervised process's log stays readable. common/'s own
// logger is quieted too while common/ is still linked.
void onyx_log_set_quiet();
