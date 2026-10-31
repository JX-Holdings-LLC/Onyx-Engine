#include "llama_util.h"

#include "log.h"          // common_log_set_verbosity_thold, while common/ is linked
#include "mtmd-helper.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>

std::vector<llama_token> onyx_tokenize(const llama_vocab * vocab, const std::string & text,
                                       bool add_special, bool parse_special) {
    // a first guess large enough for nearly every input; a negative return is
    // the exact size needed, so the second call cannot come up short
    std::vector<llama_token> out(text.size() + 2 * (add_special ? 1 : 0));
    int32_t n = llama_tokenize(vocab, text.data(), (int32_t) text.size(), out.data(), (int32_t) out.size(),
                               add_special, parse_special);
    if (n == std::numeric_limits<int32_t>::min()) {
        throw std::runtime_error("tokenization failed: the result exceeds the int32_t limit");
    }
    if (n < 0) {
        out.resize((size_t) -n);
        n = llama_tokenize(vocab, text.data(), (int32_t) text.size(), out.data(), (int32_t) out.size(),
                           add_special, parse_special);
        if (n < 0) {
            throw std::runtime_error("tokenization failed");
        }
    }
    out.resize((size_t) n);
    return out;
}

std::string onyx_token_to_piece(const llama_vocab * vocab, llama_token token, bool special) {
    std::string piece(16, '\0');
    int32_t n = llama_token_to_piece(vocab, token, &piece[0], (int32_t) piece.size(), 0, special);
    if (n < 0) {
        piece.resize((size_t) -n);
        n = llama_token_to_piece(vocab, token, &piece[0], (int32_t) piece.size(), 0, special);
        if (n < 0) {
            return std::string();
        }
    }
    piece.resize((size_t) n);
    return piece;
}

std::string onyx_detokenize(const llama_vocab * vocab, const std::vector<llama_token> & tokens, bool special) {
    std::string text(std::max<size_t>(16, tokens.size()), '\0');
    int32_t n = llama_detokenize(vocab, tokens.data(), (int32_t) tokens.size(), &text[0], (int32_t) text.size(),
                                 /* remove_special */ false, special);
    if (n < 0) {
        text.resize((size_t) -n);
        n = llama_detokenize(vocab, tokens.data(), (int32_t) tokens.size(), &text[0], (int32_t) text.size(),
                             false, special);
        if (n < 0) {
            return std::string();
        }
    }
    text.resize((size_t) n);
    return text;
}

void onyx_batch_clear(llama_batch & batch) {
    batch.n_tokens = 0;
}

void onyx_batch_add(llama_batch & batch, llama_token id, llama_pos pos,
                    const std::vector<llama_seq_id> & seq_ids, bool logits) {
    const int32_t i = batch.n_tokens;
    batch.token[i]    = id;
    batch.pos[i]      = pos;
    batch.n_seq_id[i] = (int32_t) seq_ids.size();
    for (size_t k = 0; k < seq_ids.size(); k++) {
        batch.seq_id[i][k] = seq_ids[k];
    }
    batch.logits[i] = logits;
    batch.n_tokens++;
}

const char * onyx_seq_rm_kind_name(onyx_seq_rm_kind kind) {
    switch (kind) {
        case ONYX_SEQ_RM_PARTIAL: return "partial";
        case ONYX_SEQ_RM_FULL:    return "full";
        case ONYX_SEQ_RM_BOUNDED: return "bounded";
        case ONYX_SEQ_RM_NONE:    break;
    }
    return "none";
}

onyx_seq_rm_kind onyx_probe_seq_rm(llama_context * ctx) {
    llama_memory_t mem = llama_get_memory(ctx);
    if (!mem) {
        return ONYX_SEQ_RM_NONE;
    }
    if (llama_n_rs_seq(ctx) > 0) {
        return ONYX_SEQ_RM_BOUNDED;
    }

    llama_memory_clear(mem, true);

    onyx_seq_rm_kind kind = ONYX_SEQ_RM_PARTIAL;
    {
        llama_batch_ext * batch = llama_batch_ext_init(ctx);
        // contexts that take a state embedding per token (MTP drafts) get
        // zeros; the call is a no-op false for every other context
        const llama_model * model = llama_get_model(ctx);
        std::vector<float> zeros((size_t) std::max(1, llama_model_n_embd_out(model)), 0.0f);
        bool ok = batch != nullptr;
        for (llama_pos pos = 0; ok && pos < 2; pos++) {
            const int32_t idx = llama_batch_ext_add_token(batch, 0, 0);
            ok = idx >= 0 && llama_batch_ext_set_pos(batch, idx, &pos);
            if (ok) {
                llama_batch_ext_set_embd_state(batch, idx, { zeros.data(), 1, zeros.size() });
            }
        }
        if (!ok || llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0) {
            kind = ONYX_SEQ_RM_NONE;
        } else if (!llama_memory_seq_rm(mem, 0, 1, -1)) {
            kind = ONYX_SEQ_RM_FULL;
        }
        if (batch) {
            llama_batch_ext_free(batch);
        }
    }

    llama_memory_clear(mem, true);
    llama_synchronize(ctx);
    return kind;
}

static void onyx_quiet_log(ggml_log_level level, const char * text, void *) {
    // GGML_LOG_LEVEL_CONT (above ERROR) continues a line, so it passes too,
    // as it always has
    if (level >= GGML_LOG_LEVEL_WARN) {
        fputs(text, stderr);
    }
}

void onyx_log_set_quiet() {
    llama_log_set(onyx_quiet_log, nullptr);
    mtmd_helper_log_set(onyx_quiet_log, nullptr);
    common_log_set_verbosity_thold(-1);
}
