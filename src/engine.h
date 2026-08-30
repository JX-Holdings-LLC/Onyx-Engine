// jx_engine: owns the llama.cpp model + context and executes inference.
//
// Concurrency model (v1): one model, one context, one generation at a time.
// The HTTP layer may call from many threads; all inference entry points
// serialize on an internal mutex. Streaming callers receive tokens through a
// callback and can cancel by returning false from it.
#pragma once

#include "args.h"

#include "chat.h"
#include "common.h"
#include "llama.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct jx_gen_params {
    std::vector<llama_token> prompt_tokens;
    int32_t     n_predict = -1;          // -1 = until EOG or context limit
    std::vector<std::string> stop;       // stop sequences (matched on text)
    common_params_sampling sampling;     // seed/temp/top_p/... and grammar
    bool        cache_prompt = true;     // reuse common KV prefix when possible
};

enum jx_finish_reason {
    JX_FINISH_STOP,     // EOG token or stop sequence
    JX_FINISH_LENGTH,   // hit n_predict or context limit
    JX_FINISH_CANCEL,   // caller aborted (client disconnect)
};

struct jx_gen_result {
    std::string      error;               // non-empty on failure; other fields invalid
    std::string      text;                // full generated text (stop seq trimmed)
    jx_finish_reason finish = JX_FINISH_STOP;
    std::string      stopping_word;       // which stop sequence fired, if any
    int32_t          n_prompt    = 0;     // prompt tokens evaluated (incl. cached)
    int32_t          n_cached    = 0;     // prompt tokens reused from KV cache
    int32_t          n_predicted = 0;     // tokens generated
    double           t_prompt_ms    = 0;
    double           t_predict_ms   = 0;
};

// Called for each visible piece of generated text. `piece` may be empty when
// text is being withheld for stop-sequence matching. Return false to cancel.
using jx_token_cb = std::function<bool(const std::string & piece)>;

class jx_engine {
public:
    jx_engine() = default;
    ~jx_engine();

    jx_engine(const jx_engine &) = delete;
    jx_engine & operator=(const jx_engine &) = delete;

    // Loads the model and creates the context. Returns false and sets
    // load_error() on failure.
    bool load(const jx_args & args);

    const std::string & load_error() const { return load_error_; }

    // --- metadata (safe after load) ---
    const std::string & alias()     const { return alias_; }
    uint32_t n_ctx()                const { return n_ctx_; }
    int32_t  n_ctx_train()          const;
    int32_t  n_embd()               const;
    uint64_t model_size_bytes()     const;
    uint64_t model_n_params()       const;
    std::string model_desc()        const;
    bool embedding_mode()           const { return embedding_mode_; }
    std::string chat_template_source() const;
    const common_chat_templates * chat_templates() const { return chat_templates_.get(); }
    const llama_vocab * vocab()     const { return vocab_; }
    const llama_model * model()     const { return model_; }

    // --- inference (each call serializes on the engine mutex) ---

    // Text generation. cb may be null for non-streaming use.
    jx_gen_result generate(const jx_gen_params & params, const jx_token_cb & cb);

    // Pooled embedding for one input. Returns empty vector on failure and
    // sets err. Only valid in embedding mode.
    std::vector<float> embed(const std::vector<llama_token> & tokens, std::string & err);

    // Tokenization helpers (thread-safe; vocab is immutable after load).
    std::vector<llama_token> tokenize(const std::string & text, bool add_special, bool parse_special = true) const;
    std::string detokenize(const std::vector<llama_token> & tokens, bool special = false) const;

private:
    // clears KV and re-evaluates from scratch when prefix reuse is off/fails
    bool decode_prompt(const std::vector<llama_token> & tokens, int32_t & n_cached, std::string & err);

    llama_model *   model_ = nullptr;
    llama_context * ctx_   = nullptr;
    const llama_vocab * vocab_ = nullptr;
    common_chat_templates_ptr chat_templates_;

    std::string alias_;
    std::string load_error_;
    uint32_t    n_ctx_          = 0;
    int32_t     n_batch_        = 0;
    bool        embedding_mode_ = false;
    int32_t     cache_reuse_    = 0;

    // tokens currently materialized in the KV cache (seq 0)
    std::vector<llama_token> cache_tokens_;

    std::mutex mutex_;
};
