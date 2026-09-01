// jx_engine: owns the llama.cpp model + context and executes inference.
//
// Concurrency model (v2): one model, one context, `--parallel` request slots.
// A dedicated engine thread runs a continuous-batching loop: every tick it
// packs a prompt chunk for each slot still prefilling plus exactly one
// next-token for each generating slot into a single llama_batch, decodes it
// once, and samples each slot from its own logits row with its own sampler.
//
// HTTP threads submit requests into a FIFO queue and block in generate()
// until the request finishes. Generated text is handed back through a
// per-request queue: the engine thread only ever pushes into it, the calling
// thread pops and invokes the token callback, so a slow SSE client can stall
// its own request but never the engine loop. Returning false from the
// callback cancels that request; the engine loop then releases its slot.
//
// Embedding mode keeps the v1 shape: one sequence, serialized on a mutex.
#pragma once

#include "args.h"

#include "chat.h"
#include "common.h"
#include "llama.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    bool             truncated   = false; // context was shifted at least once
    double           t_prompt_ms    = 0;
    double           t_predict_ms   = 0;
};

// Called for each visible piece of generated text. `piece` may be empty when
// text is being withheld for stop-sequence matching. Return false to cancel.
using jx_token_cb = std::function<bool(const std::string & piece)>;

// One in-flight generation request. Ownership is shared between the HTTP
// thread blocked in generate() and the engine loop running it.
struct jx_gen_request {
    jx_gen_params    params;
    common_sampler * smpl = nullptr;      // owned; freed when the request finishes
    bool             wants_pieces = false; // caller passed a token callback

    std::mutex              mu;
    std::condition_variable cv;
    std::deque<std::string> pieces;       // engine -> caller (text for the callback)
    bool                    cancelled = false;   // caller -> engine
    bool                    finished  = false;   // engine -> caller
    jx_gen_result           result;
};

using jx_gen_request_ptr = std::shared_ptr<jx_gen_request>;

// One parallel request slot. `seq_id` is both the slot index and the
// llama.cpp sequence id its KV lives under, exactly like llama-server.
struct jx_slot {
    enum state_t {
        JX_SLOT_IDLE,      // no request
        JX_SLOT_PREFILL,   // still feeding prompt tokens
        JX_SLOT_GENERATE,  // has a sampled token waiting to be decoded
    };

    llama_seq_id seq_id = 0;
    state_t      state  = JX_SLOT_IDLE;

    jx_gen_request_ptr req;

    // tokens currently materialized in this sequence's KV cache
    std::vector<llama_token> cache_tokens;

    std::vector<llama_token> prompt;         // prompt of the current request
    int32_t     n_past      = 0;             // KV positions filled for this seq
    int32_t     i_batch     = -1;            // row of the current batch holding our logits
    llama_token sampled     = 0;             // token waiting to be decoded
    size_t      n_sent      = 0;             // bytes of result.text already queued to the caller
    uint64_t    t_last_used = 0;             // LRU tick stamp
    int64_t     t_start     = 0;             // us, slot acquired
    int64_t     t_prompt    = 0;             // us, prompt finished
};

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
    uint32_t n_ctx_slot()           const { return n_ctx_slot_; }
    int32_t  n_parallel()           const { return n_parallel_; }
    bool     ctx_shift()            const { return ctx_shift_; }
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

    // --- inference ---

    // Text generation. Blocks until the request finishes; `cb` (may be null)
    // is invoked on the calling thread for each visible piece of text and
    // cancels the request by returning false. Up to --parallel requests run
    // concurrently; further callers queue in FIFO order.
    jx_gen_result generate(const jx_gen_params & params, const jx_token_cb & cb);

    // Pooled embedding for one input. Returns empty vector on failure and
    // sets err. Only valid in embedding mode (serialized on a mutex).
    std::vector<float> embed(const std::vector<llama_token> & tokens, std::string & err);

    // Tokenization helpers (thread-safe; vocab is immutable after load).
    std::vector<llama_token> tokenize(const std::string & text, bool add_special, bool parse_special = true) const;
    std::string detokenize(const std::vector<llama_token> & tokens, bool special = false) const;

private:
    // --- engine loop (all of these run on loop_thread_ only) ---
    void loop();
    bool admit_queued();                      // queued requests -> idle slots
    void build_batch(llama_batch & batch);    // one tick's shared batch
    bool decode_batch(llama_batch & batch);   // decode + per-slot sampling
    void on_sampled(jx_slot & slot);          // v1 per-token bookkeeping
    bool context_shift(jx_slot & slot);       // returns false if it cannot shift
    void emit(jx_slot & slot, std::string piece);
    void finish(jx_slot & slot, jx_finish_reason reason, const std::string & error = "");

    llama_model *   model_ = nullptr;
    llama_context * ctx_   = nullptr;
    const llama_vocab * vocab_ = nullptr;
    common_chat_templates_ptr chat_templates_;

    std::string alias_;
    std::string load_error_;
    uint32_t    n_ctx_          = 0;
    uint32_t    n_ctx_slot_     = 0;   // per-slot context budget
    int32_t     n_batch_        = 0;
    int32_t     n_parallel_     = 1;
    bool        embedding_mode_ = false;
    int32_t     cache_reuse_    = 0;
    bool        ctx_shift_      = false;
    int32_t     n_keep_         = 0;
    bool        add_bos_        = false;

    std::vector<jx_slot> slots_;

    // queue + wakeup for the engine loop
    std::mutex                    q_mutex_;
    std::condition_variable       q_cv_;
    std::deque<jx_gen_request_ptr> queue_;
    std::atomic<bool>             stop_{false};
    uint64_t                      tick_ = 0;
    std::thread                   loop_thread_;

    // embedding mode only
    std::mutex mutex_;
};
