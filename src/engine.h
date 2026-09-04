// onyx_engine: owns the llama.cpp model + context and executes inference.
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
//
// Multimodal requests (--mmproj) are the one exception to the batching loop:
// their prompt is prefilled through mtmd at admission time, which runs the
// vision/audio encoder and its own llama_decode calls inline (see
// prefill_media()).
#pragma once

#include "args.h"

#include "chat.h"
#include "common.h"
#include "llama.h"
#include "mtmd.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One decoded media file (PNG/JPEG/WAV/... bytes, exactly as they would sit
// on disk). onyx-engine never fetches media itself: the server decodes data:
// URIs and raw base64 into these buffers.
using onyx_media_buffer = std::vector<unsigned char>;

// One candidate token's raw (unmodified-by-sampler) model probability, as a
// natural log ("logprob"). Used both for the token actually sampled and for
// the top-N alternatives at that position.
struct onyx_prob_entry {
    llama_token token   = 0;
    std::string piece;      // common_token_to_piece(token), special=true
    float       logprob = 0.0f;
};

// Per-generated-token logprob record: the sampled token's own raw logprob
// plus up to `n_probs` top candidates by raw logit, both computed from the
// full-vocab softmax of the logits row that produced this token (§3 of the
// v2 API reference: OpenAI semantics report the *raw* model distribution,
// not the post-sampler-chain one, so a grammar-constrained low-probability
// pick still reports its true logprob).
struct onyx_token_probs {
    onyx_prob_entry              sampled;
    std::vector<onyx_prob_entry> top;
};

// Own-code reasoning-budget state machine parameters for one request (see
// onyx_slot::rb_state_t for the runtime state machine). Left default
// (`enabled = false`) when no budget is in effect (CLI/request budget == -1).
struct onyx_reasoning_budget {
    bool                      enabled = false;
    int32_t                   budget  = -1;   // tokens allowed inside the thinking block; 0 = none
    std::vector<llama_token>  start_tag;      // tokenized thinking-open tag
    std::vector<llama_token>  end_tag;        // tokenized thinking-close tag (primary/first one)
    std::vector<llama_token>  forced;         // tokens force-emitted once the budget is exceeded
                                              // (budget==0: just end_tag; budget>0: message + end_tag)
    bool                      start_in_prompt = false; // generation prompt itself ends inside thinking
};

struct onyx_gen_params {
    std::vector<llama_token> prompt_tokens;
    int32_t     n_predict = -1;          // -1 = until EOG or context limit
    std::vector<std::string> stop;       // stop sequences (matched on text)
    common_params_sampling sampling;     // seed/temp/top_p/... and grammar
    bool        cache_prompt = true;     // reuse common KV prefix when possible

    // multimodal: when `media` is non-empty the engine ignores prompt_tokens
    // and instead tokenizes `prompt_text` (which must contain one mtmd media
    // marker per buffer, in order) together with the buffers.
    std::string                  prompt_text;
    std::vector<onyx_media_buffer> media;

    // logprobs (v2): when `want_logprobs` is set, each generated token gets a
    // onyx_token_probs entry (see onyx_gen_result::probs); `n_probs` (0..25) is
    // how many top-alternative entries to include per token, independent of
    // whether logprobs are requested at all.
    bool    want_logprobs = false;
    int32_t n_probs       = 0;

    // reasoning budget (v2)
    onyx_reasoning_budget reasoning;
};

enum onyx_finish_reason {
    ONYX_FINISH_STOP,     // EOG token or stop sequence
    ONYX_FINISH_LENGTH,   // hit n_predict or context limit
    ONYX_FINISH_CANCEL,   // caller aborted (client disconnect)
};

struct onyx_gen_result {
    std::string      error;               // non-empty on failure; other fields invalid
    std::string      text;                // full generated text (stop seq trimmed)
    onyx_finish_reason finish = ONYX_FINISH_STOP;
    std::string      stopping_word;       // which stop sequence fired, if any
    int32_t          n_prompt    = 0;     // prompt tokens evaluated (incl. cached)
    int32_t          n_cached    = 0;     // prompt tokens reused from KV cache
    int32_t          n_predicted = 0;     // tokens generated
    bool             truncated   = false; // context was shifted at least once
    double           t_prompt_ms    = 0;
    double           t_predict_ms   = 0;

    // logprobs (v2): one entry per generated token whose text survived into
    // `text` (a token trimmed off by a stop sequence has no entry here; a
    // token withheld by stop-holdback still gets one, in order). Empty
    // unless the request set `want_logprobs`.
    std::vector<onyx_token_probs> probs;
};

// One piece of generated text handed from the engine loop to the caller,
// paired with the logprob entries for whichever tokens' text is fully
// included in `text` (see onyx_engine::emit; empty unless `want_logprobs`).
struct onyx_gen_piece {
    std::string                 text;
    std::vector<onyx_token_probs> probs;
};

// Called for each visible piece of generated text (and its logprobs, if
// requested). `text` may be empty when text is being withheld for
// stop-sequence matching. Return false to cancel.
using onyx_token_cb = std::function<bool(const std::string & text, const std::vector<onyx_token_probs> & probs)>;

// One in-flight generation request. Ownership is shared between the HTTP
// thread blocked in generate() and the engine loop running it.
struct onyx_gen_request {
    onyx_gen_params    params;
    common_sampler * smpl = nullptr;      // owned; freed when the request finishes
    bool             wants_pieces = false; // caller passed a token callback

    std::mutex               mu;
    std::condition_variable  cv;
    std::deque<onyx_gen_piece> pieces;      // engine -> caller (text + logprobs for the callback)
    bool                    cancelled = false;   // caller -> engine
    bool                    finished  = false;   // engine -> caller
    onyx_gen_result           result;
};

using onyx_gen_request_ptr = std::shared_ptr<onyx_gen_request>;

// One parallel request slot. `seq_id` is both the slot index and the
// llama.cpp sequence id its KV lives under, exactly like llama-server.
struct onyx_slot {
    enum state_t {
        ONYX_SLOT_IDLE,      // no request
        ONYX_SLOT_PREFILL,   // still feeding prompt tokens
        ONYX_SLOT_GENERATE,  // has a sampled token waiting to be decoded
    };

    // Own-code reasoning-budget state machine (no llama_sampler involved).
    // IDLE scans generated tokens for the start tag (rolling multi-token
    // match); COUNTING counts generated tokens and scans for a natural end
    // tag; once the count reaches the budget, FORCING emits
    // onyx_reasoning_budget::forced one token at a time instead of sampling;
    // DONE is passthrough forever after (whether reached naturally or via
    // forcing). OFF means the request has no budget in effect.
    enum rb_state_t {
        RB_OFF,
        RB_IDLE,
        RB_COUNTING,
        RB_FORCING,
        RB_DONE,
    };

    llama_seq_id seq_id = 0;
    state_t      state  = ONYX_SLOT_IDLE;

    onyx_gen_request_ptr req;

    // tokens currently materialized in this sequence's KV cache
    std::vector<llama_token> cache_tokens;

    std::vector<llama_token> prompt;         // prompt of the current request
    bool        has_media   = false;         // prefilled through mtmd, not `prompt`
    int32_t     n_past      = 0;             // KV positions filled for this seq
    int32_t     i_batch     = -1;            // row of the current batch holding our logits
    llama_token sampled     = 0;             // token waiting to be decoded
    size_t      n_sent      = 0;             // bytes of result.text already queued to the caller
    uint64_t    t_last_used = 0;             // LRU tick stamp
    int64_t     t_start     = 0;             // us, slot acquired
    int64_t     t_prompt    = 0;             // us, prompt finished

    // logprobs (v2): how many of result.probs have already been handed to
    // the caller via emit(), and how many bytes of result.text those cover
    // (== sum of their piece sizes) -- a prob entry is only delivered once
    // the full extent of its token's text has been sent, same rule stop
    // holdback uses for text itself.
    size_t n_probs_sent        = 0;
    size_t probs_covered_bytes = 0;

    // reasoning budget (v2) -- see rb_state_t above
    rb_state_t rb_state       = RB_OFF;
    int32_t    rb_count       = 0;   // tokens generated while COUNTING
    size_t     rb_forced_idx  = 0;   // next index into onyx_reasoning_budget::forced
    size_t     rb_start_match = 0;   // rolling match progress against start_tag
    size_t     rb_end_match   = 0;   // rolling match progress against end_tag
};

class onyx_engine {
public:
    onyx_engine() = default;
    ~onyx_engine();

    onyx_engine(const onyx_engine &) = delete;
    onyx_engine & operator=(const onyx_engine &) = delete;

    // Loads the model and creates the context. Returns false and sets
    // load_error() on failure.
    bool load(const onyx_args & args);

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

    // --- multimodal (--mmproj) ---
    bool has_mmproj()      const { return mctx_ != nullptr; }
    bool supports_vision() const { return mctx_ && mtmd_support_vision(mctx_); }
    bool supports_audio()  const { return mctx_ && mtmd_support_audio(mctx_); }
    // marker text the rendered prompt must carry once per media buffer
    std::string media_marker() const { return mctx_ ? mtmd_get_marker(mctx_) : std::string(); }

    std::string chat_template_source() const;
    const common_chat_templates * chat_templates() const { return chat_templates_.get(); }
    const llama_vocab * vocab()     const { return vocab_; }
    const llama_model * model()     const { return model_; }

    // --- inference ---

    // Text generation. Blocks until the request finishes; `cb` (may be null)
    // is invoked on the calling thread for each visible piece of text and
    // cancels the request by returning false. Up to --parallel requests run
    // concurrently; further callers queue in FIFO order.
    onyx_gen_result generate(const onyx_gen_params & params, const onyx_token_cb & cb);

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
    bool prefill_media(onyx_slot & slot);       // mtmd prefill; false if the slot was failed
    void sample_slot(onyx_slot & slot, int32_t tok_idx); // sample one token from fresh logits
    void on_sampled(onyx_slot & slot, const onyx_token_probs * probs); // per-token bookkeeping
    bool context_shift(onyx_slot & slot);       // returns false if it cannot shift
    void emit(onyx_slot & slot, std::string piece, std::vector<onyx_token_probs> probs = {});
    void finish(onyx_slot & slot, onyx_finish_reason reason, const std::string & error = "");

    llama_model *   model_ = nullptr;
    llama_context * ctx_   = nullptr;
    mtmd_context *  mctx_  = nullptr;   // multimodal projector, null without --mmproj
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

    std::vector<onyx_slot> slots_;

    // queue + wakeup for the engine loop
    std::mutex                    q_mutex_;
    std::condition_variable       q_cv_;
    std::deque<onyx_gen_request_ptr> queue_;
    std::atomic<bool>             stop_{false};
    uint64_t                      tick_ = 0;
    std::thread                   loop_thread_;

    // embedding mode only
    std::mutex mutex_;
};
