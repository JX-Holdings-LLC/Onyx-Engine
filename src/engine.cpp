#include "engine.h"

#include "log.h"
#include "sampling.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

jx_engine::~jx_engine() {
    if (loop_thread_.joinable()) {
        stop_ = true;
        q_cv_.notify_all();
        loop_thread_.join();
    }
    if (ctx_) {
        llama_free(ctx_);
    }
    if (model_) {
        llama_model_free(model_);
    }
}

static enum llama_load_mode pick_load_mode(bool no_mmap, bool mlock) {
    if (no_mmap && mlock) return LLAMA_LOAD_MODE_MLOCK;
    if (no_mmap)          return LLAMA_LOAD_MODE_NONE;
    if (mlock)            return LLAMA_LOAD_MODE_MMAP_MLOCK;
    return LLAMA_LOAD_MODE_AUTO;
}

static enum llama_pooling_type pick_pooling(const std::string & name, bool embedding_mode) {
    if (name == "none") return LLAMA_POOLING_TYPE_NONE;
    if (name == "mean") return LLAMA_POOLING_TYPE_MEAN;
    if (name == "cls")  return LLAMA_POOLING_TYPE_CLS;
    if (name == "last") return LLAMA_POOLING_TYPE_LAST;
    if (name == "rank") return LLAMA_POOLING_TYPE_RANK;
    if (name.empty() && embedding_mode) {
        // let the model decide; models without pooling metadata get mean so
        // /v1/embeddings always has one vector per input
        return LLAMA_POOLING_TYPE_UNSPECIFIED;
    }
    return LLAMA_POOLING_TYPE_UNSPECIFIED;
}

bool jx_engine::load(const jx_args & args) {
    alias_          = args.alias;
    embedding_mode_ = args.embedding;
    cache_reuse_    = std::max<int32_t>(0, args.cache_reuse);
    n_parallel_     = embedding_mode_ ? 1 : std::max<int32_t>(1, args.n_parallel);
    ctx_shift_      = args.ctx_shift && !embedding_mode_;
    n_keep_         = args.n_keep;

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = args.n_gpu_layers < 0 ? 999 : args.n_gpu_layers;
    mparams.load_mode    = pick_load_mode(args.no_mmap, args.mlock);

    model_ = llama_model_load_from_file(args.model_path.c_str(), mparams);
    if (!model_) {
        load_error_ = "failed to load model from '" + args.model_path + "'";
        return false;
    }
    vocab_   = llama_model_get_vocab(model_);
    add_bos_ = llama_vocab_get_add_bos(vocab_);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = args.n_ctx < 0 ? 0 : (uint32_t) args.n_ctx;
    // the batching loop needs room for one row per slot plus at least one
    // prompt token, otherwise a prefilling slot could never make progress
    cparams.n_batch   = (uint32_t) std::max({ 32, args.n_batch,  n_parallel_ + 1 });
    cparams.n_ubatch  = (uint32_t) std::max({ 32, args.n_ubatch, n_parallel_ + 1 });
    // one llama.cpp sequence per request slot; llama.cpp divides n_ctx into
    // per-sequence budgets (kv_unified stays at its default false)
    cparams.n_seq_max = (uint32_t) n_parallel_;

    const int32_t hw_threads = (int32_t) std::thread::hardware_concurrency();
    cparams.n_threads       = args.n_threads       > 0 ? args.n_threads       : std::max(1, hw_threads);
    cparams.n_threads_batch = args.n_threads_batch > 0 ? args.n_threads_batch : cparams.n_threads;

    cparams.flash_attn_type =
        args.flash_attn < 0 ? LLAMA_FLASH_ATTN_TYPE_AUTO :
        args.flash_attn > 0 ? LLAMA_FLASH_ATTN_TYPE_ENABLED
                            : LLAMA_FLASH_ATTN_TYPE_DISABLED;

    if (embedding_mode_) {
        cparams.embeddings   = true;
        cparams.pooling_type = pick_pooling(args.pooling, true);
    }

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        load_error_ = "failed to create inference context (ctx-size " + std::to_string(args.n_ctx) + ")";
        return false;
    }
    n_ctx_      = llama_n_ctx(ctx_);
    n_ctx_slot_ = llama_n_ctx_seq(ctx_);
    n_batch_    = (int32_t) cparams.n_batch;

    if (ctx_shift_) {
        llama_memory_t mem = llama_get_memory(ctx_);
        if (!mem || !llama_memory_can_shift(mem)) {
            ctx_shift_ = false;
            fprintf(stderr, "warning: context shift is not supported by this context, it will be disabled\n");
        }
    }

    std::string template_override = args.chat_template;
    if (!args.chat_template_file.empty()) {
        std::ifstream f(args.chat_template_file);
        if (!f) {
            load_error_ = "failed to read chat template file '" + args.chat_template_file + "'";
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        template_override = ss.str();
    }

    try {
        chat_templates_ = common_chat_templates_init(model_, template_override);
    } catch (const std::exception & e) {
        load_error_ = std::string("failed to initialize chat template: ") + e.what();
        return false;
    }

    if (!embedding_mode_) {
        slots_.resize((size_t) n_parallel_);
        for (int32_t i = 0; i < n_parallel_; i++) {
            slots_[(size_t) i].seq_id = (llama_seq_id) i;
        }
        loop_thread_ = std::thread([this] { loop(); });
    }

    return true;
}

int32_t  jx_engine::n_ctx_train() const { return llama_model_n_ctx_train(model_); }
int32_t  jx_engine::n_embd()      const { return llama_model_n_embd(model_); }
uint64_t jx_engine::model_size_bytes() const { return llama_model_size(model_); }
uint64_t jx_engine::model_n_params()   const { return llama_model_n_params(model_); }

std::string jx_engine::model_desc() const {
    char buf[256];
    llama_model_desc(model_, buf, sizeof(buf));
    return buf;
}

std::string jx_engine::chat_template_source() const {
    return chat_templates_ ? common_chat_templates_source(chat_templates_.get()) : "";
}

std::vector<llama_token> jx_engine::tokenize(const std::string & text, bool add_special, bool parse_special) const {
    return common_tokenize(vocab_, text, add_special, parse_special);
}

std::string jx_engine::detokenize(const std::vector<llama_token> & tokens, bool special) const {
    return common_detokenize(vocab_, tokens, special);
}

// longest suffix of `text` that is a prefix of any stop word (text held back
// from the client until we know it is not the start of a stop sequence)
static size_t stop_holdback(const std::string & text, const std::vector<std::string> & stops) {
    size_t hold = 0;
    for (const auto & stop : stops) {
        const size_t max_k = std::min(text.size(), stop.size() - 1);
        for (size_t k = max_k; k > hold; k--) {
            if (text.compare(text.size() - k, k, stop, 0, k) == 0) {
                hold = k;
                break;
            }
        }
    }
    return hold;
}

// ---------------------------------------------------------------------------
// public entry points
// ---------------------------------------------------------------------------

jx_gen_result jx_engine::generate(const jx_gen_params & params, const jx_token_cb & cb) {
    jx_gen_result res;

    if (embedding_mode_) {
        res.error = "this instance is running in embedding mode; text generation is disabled";
        return res;
    }

    auto req = std::make_shared<jx_gen_request>();
    req->params       = params;
    req->wants_pieces = (bool) cb;

    std::vector<llama_token> & prompt = req->params.prompt_tokens;
    if (prompt.empty()) {
        // llama_decode needs at least one token; fall back to BOS
        const llama_token bos = llama_vocab_bos(vocab_);
        prompt.push_back(bos >= 0 ? bos : 0);
    }
    if (prompt.size() >= n_ctx_slot_) {
        res.error = "prompt (" + std::to_string(prompt.size()) + " tokens) does not fit in the context window (" +
                    std::to_string(n_ctx_slot_) + " tokens)";
        return res;
    }

    // the sampler is built on the calling thread so that a bad grammar fails
    // the request synchronously, before it ever reaches the engine loop
    try {
        req->smpl = common_sampler_init(model_, req->params.sampling);
    } catch (const std::exception & e) {
        res.error = std::string("failed to initialize sampler: ") + e.what();
        return res;
    }
    if (!req->smpl) {
        res.error = "failed to initialize sampler (bad grammar?)";
        return res;
    }

    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        queue_.push_back(req);
    }
    q_cv_.notify_all();

    // consume generated pieces on this thread: a slow client stalls only its
    // own request, never the engine loop
    std::unique_lock<std::mutex> lock(req->mu);
    while (true) {
        req->cv.wait(lock, [&] { return !req->pieces.empty() || req->finished; });
        while (!req->pieces.empty()) {
            std::string piece = std::move(req->pieces.front());
            req->pieces.pop_front();
            if (!cb) {
                continue;
            }
            lock.unlock();
            const bool keep = cb(piece);
            lock.lock();
            if (!keep && !req->cancelled) {
                req->cancelled = true;
                q_cv_.notify_all();
            }
        }
        if (req->finished) {
            break;
        }
    }

    return req->result;
}

std::vector<float> jx_engine::embed(const std::vector<llama_token> & tokens, std::string & err) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!embedding_mode_) {
        err = "this instance is not running in embedding mode (start with --embedding)";
        return {};
    }
    if (tokens.empty()) {
        err = "input produced no tokens";
        return {};
    }
    if ((int32_t) tokens.size() > n_batch_) {
        err = "input (" + std::to_string(tokens.size()) + " tokens) exceeds the batch size (" +
              std::to_string(n_batch_) + " tokens)";
        return {};
    }
    if (tokens.size() > n_ctx_) {
        err = "input (" + std::to_string(tokens.size()) + " tokens) exceeds the context window (" +
              std::to_string(n_ctx_) + " tokens)";
        return {};
    }

    llama_memory_clear(llama_get_memory(ctx_), true);

    llama_batch batch = llama_batch_init((int32_t) tokens.size(), 0, 1);
    common_batch_clear(batch);
    for (size_t k = 0; k < tokens.size(); k++) {
        common_batch_add(batch, tokens[k], (llama_pos) k, {0}, true);
    }

    const bool encoder_only = llama_model_has_encoder(model_) && !llama_model_has_decoder(model_);
    const int32_t rc = encoder_only ? llama_encode(ctx_, batch) : llama_decode(ctx_, batch);
    llama_batch_free(batch);
    if (rc != 0) {
        err = "embedding evaluation failed";
        return {};
    }

    const int32_t dim = llama_model_n_embd_out(model_) > 0 ? llama_model_n_embd_out(model_) : n_embd();

    const float * data = nullptr;
    if (llama_pooling_type(ctx_) != LLAMA_POOLING_TYPE_NONE) {
        data = llama_get_embeddings_seq(ctx_, 0);
    } else {
        // no pooling metadata: use the last token's embedding
        data = llama_get_embeddings_ith(ctx_, -1);
    }
    if (!data) {
        err = "model returned no embeddings";
        return {};
    }

    std::vector<float> out(data, data + dim);

    // L2 normalize (matches llama-server's OpenAI-compatible behavior)
    double sum = 0.0;
    for (float v : out) {
        sum += (double) v * v;
    }
    const float norm = sum > 0.0 ? (float) std::sqrt(sum) : 0.0f;
    if (norm > 0.0f) {
        for (float & v : out) {
            v /= norm;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// engine loop (everything below runs on loop_thread_ only)
// ---------------------------------------------------------------------------

void jx_engine::loop() {
    llama_batch batch = llama_batch_init(n_batch_, 0, 1);

    while (true) {
        {
            std::unique_lock<std::mutex> lock(q_mutex_);
            q_cv_.wait(lock, [&] {
                if (stop_ || !queue_.empty()) {
                    return true;
                }
                for (const auto & slot : slots_) {
                    if (slot.state != jx_slot::JX_SLOT_IDLE) {
                        return true;
                    }
                }
                return false;
            });
            if (stop_) {
                break;
            }
        }

        // release slots whose caller gave up (cb returned false)
        for (auto & slot : slots_) {
            if (slot.state == jx_slot::JX_SLOT_IDLE) {
                continue;
            }
            bool cancelled = false;
            {
                std::lock_guard<std::mutex> lock(slot.req->mu);
                cancelled = slot.req->cancelled;
            }
            if (cancelled) {
                finish(slot, JX_FINISH_CANCEL);
            }
        }

        admit_queued();

        build_batch(batch);
        if (batch.n_tokens == 0) {
            continue;
        }
        decode_batch(batch);
    }

    llama_batch_free(batch);

    // unblock anything still waiting on shutdown
    for (auto & slot : slots_) {
        if (slot.state != jx_slot::JX_SLOT_IDLE) {
            finish(slot, JX_FINISH_CANCEL, "engine is shutting down");
        }
    }
    std::deque<jx_gen_request_ptr> pending;
    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        pending.swap(queue_);
    }
    for (auto & req : pending) {
        if (req->smpl) {
            common_sampler_free(req->smpl);
            req->smpl = nullptr;
        }
        req->result.error = "engine is shutting down";
        {
            std::lock_guard<std::mutex> lock(req->mu);
            req->finished = true;
        }
        req->cv.notify_all();
    }
}

bool jx_engine::admit_queued() {
    bool admitted = false;

    while (true) {
        jx_gen_request_ptr req;
        {
            std::lock_guard<std::mutex> lock(q_mutex_);
            if (queue_.empty()) {
                break;
            }
            req = queue_.front();
        }

        // pick the idle slot whose cached tokens share the longest prefix with
        // this prompt; ties (and the no-reuse case) fall back to LRU
        jx_slot * best     = nullptr;
        size_t    best_lcp = 0;
        for (auto & slot : slots_) {
            if (slot.state != jx_slot::JX_SLOT_IDLE) {
                continue;
            }
            size_t lcp = 0;
            if (cache_reuse_ > 0 && req->params.cache_prompt) {
                const size_t max_lcp = std::min(slot.cache_tokens.size(), req->params.prompt_tokens.size() - 1);
                while (lcp < max_lcp && slot.cache_tokens[lcp] == req->params.prompt_tokens[lcp]) {
                    lcp++;
                }
            }
            if (!best || lcp > best_lcp || (lcp == best_lcp && slot.t_last_used < best->t_last_used)) {
                best     = &slot;
                best_lcp = lcp;
            }
        }
        if (!best) {
            // all slots busy: the head of the queue waits, and nothing behind
            // it jumps ahead (FIFO)
            break;
        }

        {
            std::lock_guard<std::mutex> lock(q_mutex_);
            queue_.pop_front();
        }

        jx_slot & slot = *best;
        slot.req      = req;
        slot.prompt   = req->params.prompt_tokens;
        slot.state    = jx_slot::JX_SLOT_PREFILL;
        slot.i_batch  = -1;
        slot.n_sent   = 0;
        slot.t_start  = ggml_time_us();
        slot.t_prompt = 0;

        req->result.n_prompt = (int32_t) slot.prompt.size();

        // trim this sequence's KV back to the reusable prefix, leaving at
        // least one prompt token to evaluate so there are fresh logits
        llama_memory_t mem = llama_get_memory(ctx_);
        size_t prefix = 0;
        if (cache_reuse_ > 0 && req->params.cache_prompt && (int32_t) best_lcp >= cache_reuse_) {
            prefix = best_lcp;
        }
        if (prefix > 0) {
            llama_memory_seq_rm(mem, slot.seq_id, (llama_pos) prefix, -1);
            slot.cache_tokens.resize(prefix);
        } else {
            llama_memory_seq_rm(mem, slot.seq_id, -1, -1);
            slot.cache_tokens.clear();
        }
        req->result.n_cached = (int32_t) prefix;
        slot.n_past          = (int32_t) prefix;

        admitted = true;
    }

    return admitted;
}

void jx_engine::build_batch(llama_batch & batch) {
    common_batch_clear(batch);
    for (auto & slot : slots_) {
        slot.i_batch = -1;
    }

    // exactly one next-token row per generating slot
    for (auto & slot : slots_) {
        if (slot.state != jx_slot::JX_SLOT_GENERATE) {
            continue;
        }
        if (batch.n_tokens >= n_batch_) {
            break;
        }
        slot.i_batch = batch.n_tokens;
        common_batch_add(batch, slot.sampled, slot.n_past, { slot.seq_id }, true);
        slot.cache_tokens.push_back(slot.sampled);
        slot.n_past++;
    }

    // fill the remainder with prompt chunks, split evenly across the slots
    // that are still prefilling
    int32_t n_prefilling = 0;
    for (const auto & slot : slots_) {
        if (slot.state == jx_slot::JX_SLOT_PREFILL) {
            n_prefilling++;
        }
    }
    if (n_prefilling == 0) {
        return;
    }

    const int32_t budget   = n_batch_ - batch.n_tokens;
    const int32_t per_slot = std::max(1, budget / n_prefilling);

    for (auto & slot : slots_) {
        if (slot.state != jx_slot::JX_SLOT_PREFILL) {
            continue;
        }
        const int32_t n_prompt = (int32_t) slot.prompt.size();
        const int32_t n        = std::min({ n_prompt - slot.n_past, per_slot, n_batch_ - batch.n_tokens });
        for (int32_t k = 0; k < n; k++) {
            const bool last = slot.n_past == n_prompt - 1;
            if (last) {
                slot.i_batch = batch.n_tokens;
            }
            common_batch_add(batch, slot.prompt[(size_t) slot.n_past], slot.n_past, { slot.seq_id }, last);
            slot.cache_tokens.push_back(slot.prompt[(size_t) slot.n_past]);
            slot.n_past++;
        }
    }
}

bool jx_engine::decode_batch(llama_batch & batch) {
    int32_t n_view = n_batch_;

    for (int32_t off = 0; off < batch.n_tokens; ) {
        const int32_t n = std::min(n_view, batch.n_tokens - off);

        llama_batch view = {
            /* n_tokens */ n,
            /* token    */ batch.token    + off,
            /* embd     */ nullptr,
            /* pos      */ batch.pos      + off,
            /* n_seq_id */ batch.n_seq_id + off,
            /* seq_id   */ batch.seq_id   + off,
            /* logits   */ batch.logits   + off,
        };

        const int32_t ret = llama_decode(ctx_, view);
        if (ret == 1 && n > 1) {
            // no KV slot for this batch size: retry the same offset smaller
            n_view = n / 2;
            continue;
        }
        if (ret != 0) {
            const std::string err =
                ret ==  1 ? "context size has been exceeded (llama_decode)" :
                ret == -1 ? "invalid input batch (llama_decode)"
                          : "compute error (llama_decode)";
            llama_memory_t mem = llama_get_memory(ctx_);
            for (auto & slot : slots_) {
                if (slot.state == jx_slot::JX_SLOT_IDLE) {
                    continue;
                }
                llama_memory_seq_rm(mem, slot.seq_id, -1, -1);
                slot.cache_tokens.clear();
                slot.n_past = 0;
                finish(slot, JX_FINISH_STOP, err);
            }
            return false;
        }

        // sample for every slot whose logits row landed in this view
        for (auto & slot : slots_) {
            if (slot.i_batch < off || slot.i_batch >= off + n) {
                continue;
            }
            const int32_t tok_idx = slot.i_batch - off;
            slot.i_batch = -1;

            jx_gen_request & req = *slot.req;
            jx_gen_result  & res = req.result;

            if (slot.state == jx_slot::JX_SLOT_PREFILL) {
                slot.state    = jx_slot::JX_SLOT_GENERATE;
                slot.t_prompt = ggml_time_us();
                res.t_prompt_ms = (slot.t_prompt - slot.t_start) / 1000.0;
            }

            // v1 ordering: budget checks first, then sample from the logits
            // this decode just produced
            if (req.params.n_predict >= 0 && res.n_predicted >= req.params.n_predict) {
                finish(slot, JX_FINISH_LENGTH);
                continue;
            }
            if (slot.n_past >= (int32_t) n_ctx_slot_) {
                if (!ctx_shift_ || !context_shift(slot)) {
                    finish(slot, JX_FINISH_LENGTH);
                    continue;
                }
            }

            const llama_token tok = common_sampler_sample(req.smpl, ctx_, tok_idx);
            common_sampler_accept(req.smpl, tok, true);
            res.n_predicted++;
            slot.sampled = tok;

            on_sampled(slot);
        }

        off += n;
        n_view = n_batch_;
    }

    return true;
}

// EOG / stop-sequence / stop-holdback handling for one freshly sampled token.
// Mirrors v1's per-token block exactly, per slot.
void jx_engine::on_sampled(jx_slot & slot) {
    jx_gen_request & req = *slot.req;
    jx_gen_result  & res = req.result;

    if (llama_vocab_is_eog(vocab_, slot.sampled)) {
        finish(slot, JX_FINISH_STOP);
        return;
    }

    res.text += common_token_to_piece(ctx_, slot.sampled);

    for (const auto & stop : req.params.stop) {
        const size_t pos = res.text.find(stop, slot.n_sent > stop.size() ? slot.n_sent - stop.size() : 0);
        if (pos != std::string::npos) {
            res.text.resize(pos);
            res.stopping_word = stop;
            finish(slot, JX_FINISH_STOP);
            return;
        }
    }

    if (req.wants_pieces) {
        const size_t hold = stop_holdback(res.text, req.params.stop);
        if (res.text.size() - hold > slot.n_sent) {
            std::string piece = res.text.substr(slot.n_sent, res.text.size() - hold - slot.n_sent);
            slot.n_sent += piece.size();
            emit(slot, std::move(piece));
        }
    }
}

// Drops the oldest half of this slot's non-preserved context, exactly as
// llama-server does, so generation can continue past the context limit.
bool jx_engine::context_shift(jx_slot & slot) {
    const int32_t n_ctx_s = (int32_t) n_ctx_slot_;

    int32_t n_keep = n_keep_ < 0 ? (int32_t) slot.prompt.size() : n_keep_;
    if (add_bos_) {
        n_keep += 1;
    }
    n_keep = std::min(n_ctx_s - 4, n_keep);
    n_keep = std::max(0, n_keep);

    const int32_t n_left    = slot.n_past - n_keep;
    int32_t       n_discard = std::clamp(n_left / 2, 0, std::max(0, n_left - 1));
    if (n_discard <= 0) {
        return false;
    }

    llama_memory_t mem = llama_get_memory(ctx_);
    llama_memory_seq_rm (mem, slot.seq_id, n_keep, n_keep + n_discard);
    llama_memory_seq_add(mem, slot.seq_id, n_keep + n_discard, slot.n_past, -n_discard);

    for (size_t i = (size_t) (n_keep + n_discard); i < slot.cache_tokens.size(); i++) {
        slot.cache_tokens[i - (size_t) n_discard] = slot.cache_tokens[i];
    }
    slot.cache_tokens.resize(slot.cache_tokens.size() - (size_t) n_discard);
    slot.n_past -= n_discard;

    slot.req->result.truncated = true;
    return true;
}

void jx_engine::emit(jx_slot & slot, std::string piece) {
    jx_gen_request & req = *slot.req;
    {
        std::lock_guard<std::mutex> lock(req.mu);
        req.pieces.push_back(std::move(piece));
    }
    req.cv.notify_all();
}

void jx_engine::finish(jx_slot & slot, jx_finish_reason reason, const std::string & error) {
    jx_gen_request_ptr req = slot.req;
    jx_gen_result &    res = req->result;

    res.finish = reason;
    if (!error.empty()) {
        res.error = error;
    }

    const int64_t now = ggml_time_us();
    if (slot.t_prompt == 0) {
        slot.t_prompt   = now;
        res.t_prompt_ms = (now - slot.t_start) / 1000.0;
    }
    res.t_predict_ms = (now - slot.t_prompt) / 1000.0;

    // hand over any text withheld for stop-sequence matching
    if (req->wants_pieces && res.error.empty() && reason != JX_FINISH_CANCEL && res.text.size() > slot.n_sent) {
        emit(slot, res.text.substr(slot.n_sent));
        slot.n_sent = res.text.size();
    }

    if (req->smpl) {
        common_sampler_free(req->smpl);
        req->smpl = nullptr;
    }

    slot.req.reset();
    slot.state       = jx_slot::JX_SLOT_IDLE;
    slot.prompt.clear();
    slot.i_batch     = -1;
    slot.n_sent      = 0;
    slot.t_prompt    = 0;
    slot.t_last_used = ++tick_;

    {
        std::lock_guard<std::mutex> lock(req->mu);
        req->finished = true;
    }
    req->cv.notify_all();
}
