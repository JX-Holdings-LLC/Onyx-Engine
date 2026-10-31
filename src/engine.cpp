#include "engine.h"
#include "llama_util.h"

#include "mtmd-helper.h"

#include "llama-cpp.h"
// staging API: llama_batch_ext_set_decision_order (see CMakeLists.txt)
#include "llama-ext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <thread>

onyx_engine::~onyx_engine() {
    if (loop_thread_.joinable()) {
        {
            // must hold q_mutex_ so the store cannot slip between the loop's
            // predicate check and its wait (lost wakeup -> join hangs)
            std::lock_guard<std::mutex> lock(q_mutex_);
            stop_ = true;
        }
        q_cv_.notify_all();
        loop_thread_.join();
    }
    if (mctx_) {
        mtmd_free(mctx_);
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

static enum ggml_type pick_cache_type(const std::string & name) {
    // the parser accepts exactly these names (onyx_cache_type_is_valid)
    if (name == "f32")    return GGML_TYPE_F32;
    if (name == "bf16")   return GGML_TYPE_BF16;
    if (name == "q8_0")   return GGML_TYPE_Q8_0;
    if (name == "q4_0")   return GGML_TYPE_Q4_0;
    if (name == "q4_1")   return GGML_TYPE_Q4_1;
    if (name == "iq4_nl") return GGML_TYPE_IQ4_NL;
    if (name == "q5_0")   return GGML_TYPE_Q5_0;
    if (name == "q5_1")   return GGML_TYPE_Q5_1;
    return GGML_TYPE_F16;
}

// The MoE expert tensors, as llama-server names them (common.h's
// LLM_FFN_EXPS_REGEX); libllama matches each pattern with std::regex_search.
static const char * const ONYX_FFN_EXPS_REGEX = "\\.ffn_(up|down|gate|gate_up)_(ch|)exps";

bool onyx_engine::build_tensor_overrides(const onyx_args & args) {
    tensor_override_patterns_.clear();
    tensor_overrides_.clear();
    if (args.tensor_overrides.empty() && !args.cpu_moe && args.n_cpu_moe <= 0) {
        return true;
    }

    // buffer types by name, from every registered device (llama-server's
    // -ot lookup); "CPU" is the host's
    std::vector<std::pair<std::string, ggml_backend_buffer_type_t>> bufts;
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_buffer_type_t buft = ggml_backend_dev_buffer_type(ggml_backend_dev_get(i));
        if (buft) {
            bufts.emplace_back(ggml_backend_buft_name(buft), buft);
        }
    }

    // command-line order, then the MoE flags: libllama takes the first
    // pattern that matches a tensor, so an explicit -ot wins
    std::vector<ggml_backend_buffer_type_t> targets;
    for (const auto & [pattern, name] : args.tensor_overrides) {
        ggml_backend_buffer_type_t buft = nullptr;
        for (const auto & [n, b] : bufts) {
            if (n == name) {
                buft = b;
            }
        }
        if (!buft) {
            std::string known;
            for (const auto & entry : bufts) {
                known += (known.empty() ? "" : ", ") + entry.first;
            }
            load_error_ = "--override-tensor: unknown buffer type '" + name + "' (available: " + known + ")";
            return false;
        }
        tensor_override_patterns_.push_back(pattern);
        targets.push_back(buft);
    }
    if (args.cpu_moe) {
        tensor_override_patterns_.push_back(ONYX_FFN_EXPS_REGEX);
        targets.push_back(ggml_backend_cpu_buffer_type());
    } else {
        for (int32_t il = 0; il < args.n_cpu_moe; il++) {
            tensor_override_patterns_.push_back("blk\\." + std::to_string(il) + ONYX_FFN_EXPS_REGEX);
            targets.push_back(ggml_backend_cpu_buffer_type());
        }
    }

    // the strings above live as long as the engine; libllama reads the
    // null-terminated array during the load
    for (size_t i = 0; i < tensor_override_patterns_.size(); i++) {
        tensor_overrides_.push_back({ tensor_override_patterns_[i].c_str(), targets[i] });
    }
    tensor_overrides_.push_back({ nullptr, nullptr });
    return true;
}

bool onyx_engine::load(const onyx_args & args) {
    alias_          = args.alias;
    embedding_mode_ = args.embedding;
    cache_reuse_    = std::max<int32_t>(0, args.cache_reuse);
    n_parallel_     = embedding_mode_ ? 1 : std::max<int32_t>(1, args.n_parallel);
    ctx_shift_      = args.ctx_shift && !embedding_mode_;
    n_keep_         = args.n_keep;

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = args.n_gpu_layers < 0 ? 999 : args.n_gpu_layers;
    mparams.load_mode    = pick_load_mode(args.no_mmap, args.mlock);
    if (!build_tensor_overrides(args)) {
        return false;
    }
    if (!tensor_overrides_.empty()) {
        mparams.tensor_buft_overrides = tensor_overrides_.data();
    }

    model_ = llama_model_load_from_file(args.model_path.c_str(), mparams);
    if (!model_) {
        load_error_ = "failed to load model from '" + args.model_path + "'";
        return false;
    }
    vocab_   = llama_model_get_vocab(model_);
    add_bos_ = llama_vocab_get_add_bos(vocab_);

    // a decision model serves /v1/systemone only: one request at a time, no
    // batching loop, and two sequences (a shared prefix and one question)
    const onyx_decision_type decision_type = onyx_decision_type_of(model_);
    const bool is_decision = decision_type != ONYX_DECISION_NONE;
    if (is_decision && embedding_mode_) {
        load_error_ = "--embedding cannot be used with a decision model (it serves /v1/systemone)";
        return false;
    }
    if (is_decision) {
        n_parallel_        = 1;
        ctx_shift_         = false;
        decision_reuse_on_ = args.cache_reuse > 0;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = args.n_ctx < 0 ? 0 : (uint32_t) args.n_ctx;
    // the batching loop needs room for one row per slot plus at least one
    // prompt token, otherwise a prefilling slot could never make progress
    cparams.n_batch   = (uint32_t) std::max({ 32, args.n_batch,  n_parallel_ + 1 });
    cparams.n_ubatch  = (uint32_t) (embedding_mode_ ? cparams.n_batch : std::max({ 32, args.n_ubatch, n_parallel_ + 1 }));
    // one llama.cpp sequence per request slot; llama.cpp divides n_ctx into
    // per-sequence budgets (kv_unified stays at its default false)
    cparams.n_seq_max = (uint32_t) n_parallel_;
    if (is_decision) {
        // both sequences see the whole context instead of half of it each
        cparams.n_seq_max  = 2;
        cparams.kv_unified = true;
    }

    const int32_t hw_threads = (int32_t) std::thread::hardware_concurrency();
    cparams.n_threads       = args.n_threads       > 0 ? args.n_threads       : std::max(1, hw_threads);
    cparams.n_threads_batch = args.n_threads_batch > 0 ? args.n_threads_batch : cparams.n_threads;

    cparams.flash_attn_type =
        args.flash_attn < 0 ? LLAMA_FLASH_ATTN_TYPE_AUTO :
        args.flash_attn > 0 ? LLAMA_FLASH_ATTN_TYPE_ENABLED
                            : LLAMA_FLASH_ATTN_TYPE_DISABLED;

    // KV cache element types; libllama turns flash attention on for a
    // quantized V cache under -fa auto (the parser refuses it with -fa off)
    cparams.type_k = pick_cache_type(args.cache_type_k);
    cparams.type_v = pick_cache_type(args.cache_type_v);

    if (embedding_mode_) {
        cparams.embeddings   = true;
        cparams.pooling_type = pick_pooling(args.pooling, true);
    }
    if (is_decision && !onyx_decision_reads_embeddings(decision_type)) {
        // label families read one logits row per decode; llama.cpp wants
        // room for one per sequence
        cparams.n_outputs_max = cparams.n_seq_max;
    }
    if (onyx_decision_reads_embeddings(decision_type)) {
        // these families answer from per-token embeddings rows, read from
        // the one batch that holds them all
        cparams.embeddings   = true;
        cparams.pooling_type = LLAMA_POOLING_TYPE_NONE;
        cparams.n_ubatch     = cparams.n_batch;
    }

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        load_error_ = "failed to create inference context (ctx-size " + std::to_string(args.n_ctx);
        if (cparams.type_k != GGML_TYPE_F16 || cparams.type_v != GGML_TYPE_F16) {
            // the usual cause: the head size is not a multiple of the type's block size
            load_error_ += ", cache types " + args.cache_type_k + "/" + args.cache_type_v;
        }
        load_error_ += ")";
        return false;
    }
    n_ctx_      = llama_n_ctx(ctx_);
    n_ctx_slot_ = llama_n_ctx_seq(ctx_);
    n_batch_    = (int32_t) cparams.n_batch;
    tick_tokens_ = args.tick_tokens > 0 ? std::min(args.tick_tokens, n_batch_) : 0;
    max_queue_   = std::max<int32_t>(0, args.max_queue);
    queue_timeout_us_ = (int64_t) std::max<int32_t>(0, args.queue_timeout) * 1000000;
    // Prefix reuse stays on for sliding-window and recurrent memory: whether a
    // slot's state can be cut back to a shared prefix is decided per request
    // in admit_queued() (llama-server's rule). With swa_full (llama.cpp's
    // default, kept here) the SWA cache holds every position, so the window
    // never limits reuse.
    n_swa_ = cparams.swa_full ? 0 : llama_model_n_swa(model_);

    if (!args.mmproj_path.empty()) {
        mtmd_context_params mparams_mtmd = mtmd_context_params_default();
        mparams_mtmd.use_gpu       = args.n_gpu_layers != 0;
        mparams_mtmd.print_timings = args.verbose;
        mparams_mtmd.n_threads     = (int) cparams.n_threads;
        mparams_mtmd.flash_attn_type = cparams.flash_attn_type;

        mctx_ = mtmd_init_from_file(args.mmproj_path.c_str(), model_, mparams_mtmd);
        if (!mctx_) {
            load_error_ = "failed to load multimodal projector from '" + args.mmproj_path + "' (--mmproj)";
            return false;
        }

        // a media chunk occupies KV positions that cannot be prefix-matched
        // token-by-token nor partially discarded, so both optimizations that
        // splice a sequence's KV are off for the whole process (mirrors
        // llama-server's load-time force-disable)
        if (cache_reuse_ > 0) {
            cache_reuse_ = 0;
            fprintf(stderr, "warning: cache_reuse is not supported by multimodal, it will be disabled\n");
        }
        if (ctx_shift_) {
            ctx_shift_ = false;
            fprintf(stderr, "warning: context shift is not supported by multimodal, it will be disabled\n");
        }
    }

    if (ctx_shift_) {
        llama_memory_t mem = llama_get_memory(ctx_);
        if (!mem || !llama_memory_can_shift(mem)) {
            ctx_shift_ = false;
            fprintf(stderr, "warning: context shift is not supported by this context, it will be disabled\n");
        }
    }

    std::string template_override = args.chat_template;
    if (!template_override.empty() && template_override != "chatml" &&
        template_override.find("{{") == std::string::npos &&
        template_override.find("{%") == std::string::npos) {
        load_error_ = "--chat-template expects Jinja source or 'chatml'; use --chat-template-file for a template file";
        return false;
    }
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
        chat_templates_ = std::make_unique<onyx_chat_templates>(model_, template_override);
    } catch (const std::exception & e) {
        load_error_ = std::string("failed to initialize chat template: ") + e.what();
        return false;
    }

    if (is_decision) {
        try {
            decision_.init(model_, media_marker());
        } catch (const std::exception & e) {
            load_error_ = std::string("failed to set up the decision model: ") + e.what();
            return false;
        }
        fprintf(stderr, "decision model (%s): serving /v1/systemone\n", onyx_decision_type_name(decision_type));
        return true;
    }

    if (!embedding_mode_) {
        // before any slot holds state: the probe clears the memory
        seq_rm_ = onyx_probe_seq_rm(ctx_);
        if (args.verbose) {
            fprintf(stderr, "memory: sequence removal is %s\n", onyx_seq_rm_kind_name(seq_rm_));
        }
        slots_.resize((size_t) n_parallel_);
        for (int32_t i = 0; i < n_parallel_; i++) {
            slots_[(size_t) i].seq_id = (llama_seq_id) i;
        }
        loop_thread_ = std::thread([this] { loop(); });
    }

    return true;
}

int32_t  onyx_engine::n_ctx_train() const { return llama_model_n_ctx_train(model_); }
int32_t  onyx_engine::n_embd()      const { return llama_model_n_embd(model_); }
uint64_t onyx_engine::model_size_bytes() const { return llama_model_size(model_); }
uint64_t onyx_engine::model_n_params()   const { return llama_model_n_params(model_); }

std::string onyx_engine::model_desc() const {
    char buf[256];
    llama_model_desc(model_, buf, sizeof(buf));
    return buf;
}

std::string onyx_engine::chat_template_source() const {
    return chat_templates_ ? chat_templates_->source() : "";
}

std::vector<llama_token> onyx_engine::tokenize(const std::string & text, bool add_special, bool parse_special) const {
    return onyx_tokenize(vocab_, text, add_special, parse_special);
}

std::vector<llama_token> onyx_engine::tokenize_prompt(const onyx_prompt & prompt) const {
    // template text is parsed for special tokens; text that came from the
    // request (message content) is not, so a client cannot write "<|im_end|>"
    // and get the control token. BOS, if the vocab adds one, comes once.
    std::vector<llama_token> out;
    bool first = true;
    for (const auto & part : prompt.parts) {
        const auto toks = onyx_tokenize(vocab_, part.text, /* add_special */ first, /* parse_special */ !part.is_input);
        out.insert(out.end(), toks.begin(), toks.end());
        first = false;
    }
    if (first) {
        out = onyx_tokenize(vocab_, "", true, true);
    }
    return out;
}

std::string onyx_engine::detokenize(const std::vector<llama_token> & tokens, bool special) const {
    return onyx_detokenize(vocab_, tokens, special);
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

// Raw (unmodified-by-sampler-chain) model probability of `token` at logits
// row `tok_idx` of the just-decoded batch/view (-1 = the last row), plus the
// top `n_probs` alternatives -- a numerically-stable single-pass softmax over
// the FULL vocab (not just the reported top-N), matching OpenAI's
// logprobs semantics: report the raw distribution even when a grammar or
// other constraint picked a token that had very low raw probability.
static onyx_token_probs compute_token_probs(llama_context * ctx, const llama_vocab * vocab,
                                          int32_t tok_idx, llama_token token, int32_t n_probs,
                                          const std::set<llama_token> & preserved_tokens) {
    onyx_token_probs out;

    const int32_t n_vocab = llama_vocab_n_tokens(vocab);
    const float * logits  = llama_get_logits_ith(ctx, tok_idx);

    float max_l = -std::numeric_limits<float>::infinity();
    for (int32_t i = 0; i < n_vocab; i++) {
        max_l = std::max(max_l, logits[i]);
    }
    double sum = 0.0;
    for (int32_t i = 0; i < n_vocab; i++) {
        sum += (double) expf(logits[i] - max_l);
    }
    const double log_sum = std::log(sum);

    auto logprob_of = [&](llama_token t) -> float {
        return (float) (((double) logits[t] - (double) max_l) - log_sum);
    };

    out.sampled.token   = token;
    out.sampled.piece   = onyx_token_to_piece(vocab, token, preserved_tokens.count(token) > 0);
    out.sampled.logprob = logprob_of(token);

    const int32_t n_top = std::max<int32_t>(0, std::min(n_probs, n_vocab));
    if (n_top > 0) {
        std::vector<int32_t> idx(n_vocab);
        for (int32_t i = 0; i < n_vocab; i++) {
            idx[i] = i;
        }
        std::partial_sort(idx.begin(), idx.begin() + n_top, idx.end(),
            [&](int32_t a, int32_t b) { return logits[a] > logits[b]; });

        out.top.reserve((size_t) n_top);
        for (int32_t i = 0; i < n_top; i++) {
            onyx_prob_entry e;
            e.token   = (llama_token) idx[(size_t) i];
            e.piece   = onyx_token_to_piece(vocab, e.token, preserved_tokens.count(e.token) > 0);
            e.logprob = logprob_of(e.token);
            out.top.push_back(std::move(e));
        }
    }

    return out;
}

// Returns (and marks delivered) the prefix of `res.probs`, starting after
// whatever was already delivered, whose tokens' text lies entirely within
// the first `slot.n_sent` bytes of `res.text` -- i.e. exactly the prob
// entries for tokens whose piece has now been fully flushed to the caller.
// A token held back (in full or in part) by stop-holdback is not included
// until a later call once the rest of its text has been sent.
static std::vector<onyx_token_probs> collect_delivered_probs(onyx_slot & slot, onyx_gen_result & res) {
    std::vector<onyx_token_probs> out;
    size_t covered = slot.probs_covered_bytes;
    while (slot.n_probs_sent < res.probs.size()) {
        const size_t next_covered = covered + res.probs[slot.n_probs_sent].sampled.piece.size();
        if (next_covered > slot.n_sent) {
            break;
        }
        out.push_back(res.probs[slot.n_probs_sent]);
        covered = next_covered;
        slot.n_probs_sent++;
    }
    slot.probs_covered_bytes = covered;
    return out;
}

// ---------------------------------------------------------------------------
// public entry points
// ---------------------------------------------------------------------------

bool onyx_engine::queue_full() {
    if (max_queue_ <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(q_mutex_);
    return (int32_t) queue_.size() >= max_queue_;
}

onyx_engine_metrics onyx_engine::metrics() {
    onyx_engine_metrics m;
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        m = metrics_;
    }
    std::lock_guard<std::mutex> lock(q_mutex_);
    m.requests_deferred = (int32_t) queue_.size();
    return m;
}

onyx_gen_result onyx_engine::generate(const onyx_gen_params & params, const onyx_token_cb & cb) {
    onyx_gen_result res;

    if (embedding_mode_) {
        res.error = "this instance is running in embedding mode; text generation is disabled";
        return res;
    }

    if (!params.media.empty() && !mctx_) {
        res.error = "this instance has no multimodal projector loaded (start with --mmproj)";
        return res;
    }

    auto req = std::make_shared<onyx_gen_request>();
    req->params       = params;
    req->wants_pieces = (bool) cb;

    if (req->params.media.empty()) {
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
        for (llama_token token : prompt) {
            if (token < 0 || token >= llama_vocab_n_tokens(vocab_)) {
                res.error = "prompt contains an invalid token id";
                return res;
            }
        }
    }

    req->params.n_probs = std::clamp<int32_t>(req->params.n_probs, 0, 25);

    // the sampler is built on the calling thread so that a bad grammar fails
    // the request synchronously, before it ever reaches the engine loop
    try {
        req->smpl = std::make_unique<onyx_sampler>(model_, req->params.sampling);
    } catch (const std::exception & e) {
        res.error = std::string("failed to initialize sampler: ") + e.what();
        return res;
    }

    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        if (max_queue_ > 0 && (int32_t) queue_.size() >= max_queue_) {
            req->smpl.reset();
            {
                std::lock_guard<std::mutex> m_lock(metrics_mutex_);
                metrics_.requests_rejected++;
            }
            res.error      = "server is busy: " + std::to_string(max_queue_) +
                             " requests are already waiting for a slot (--max-queue)";
            res.queue_full = true;
            return res;
        }
        req->t_queued = ggml_time_us();
        queue_.push_back(req);
    }
    q_cv_.notify_all();

    // consume generated pieces on this thread: a slow client stalls only its
    // own request, never the engine loop
    std::unique_lock<std::mutex> lock(req->mu);
    while (true) {
        req->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !req->pieces.empty() || req->finished; });
        if (cb && !req->finished && req->pieces.empty()) {
            lock.unlock();
            const bool keep = cb("", {});
            lock.lock();
            if (!keep && !req->cancelled) {
                req->cancelled = true;
                q_cv_.notify_all();
            }
        }
        while (!req->pieces.empty()) {
            onyx_gen_piece piece = std::move(req->pieces.front());
            req->pieces.pop_front();
            if (!cb) {
                continue;
            }
            lock.unlock();
            const bool keep = cb(piece.text, piece.probs);
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

std::vector<float> onyx_engine::embed(const std::vector<llama_token> & tokens, std::string & err) {
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
    for (llama_token token : tokens) {
        if (token < 0 || token >= llama_vocab_n_tokens(vocab_)) {
            err = "input contains an invalid token id";
            return {};
        }
    }
    if (tokens.size() > n_ctx_) {
        err = "input (" + std::to_string(tokens.size()) + " tokens) exceeds the context window (" +
              std::to_string(n_ctx_) + " tokens)";
        return {};
    }

    llama_memory_clear(llama_get_memory(ctx_), true);

    llama_batch batch = llama_batch_init((int32_t) tokens.size(), 0, 1);
    onyx_batch_clear(batch);
    for (size_t k = 0; k < tokens.size(); k++) {
        onyx_batch_add(batch, tokens[k], (llama_pos) k, {0}, true);
    }

    const bool encoder_only = llama_model_has_encoder(model_) && !llama_model_has_decoder(model_);
    const int32_t rc = encoder_only ? llama_encode(ctx_, batch) : llama_decode(ctx_, batch);
    llama_batch_free(batch);
    if (rc != 0) {
        err = "embedding evaluation failed";
        return {};
    }

    // Rank pooling (rerankers) yields n_cls_out classifier scores per sequence,
    // not an n_embd vector: reading n_embd floats would run past llama.cpp's
    // per-sequence buffer, and L2 normalization would destroy the score.
    const bool rank = llama_pooling_type(ctx_) == LLAMA_POOLING_TYPE_RANK;

    int32_t dim = 0;
    if (rank) {
        dim = llama_model_n_cls_out(model_) > 0 ? (int32_t) llama_model_n_cls_out(model_) : 1;
    } else {
        dim = llama_model_n_embd_out(model_) > 0 ? llama_model_n_embd_out(model_) : n_embd();
    }

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
    if (rank) {
        // raw relevance score(s), unnormalized, as llama-server's /rerank reports them
        return out;
    }

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

void onyx_engine::loop() {
    llama_batch batch = llama_batch_init(n_batch_, 0, 1);

    while (true) {
        {
            std::unique_lock<std::mutex> lock(q_mutex_);
            q_cv_.wait(lock, [&] {
                if (stop_ || !queue_.empty()) {
                    return true;
                }
                for (const auto & slot : slots_) {
                    if (slot.state != onyx_slot::ONYX_SLOT_IDLE) {
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
            if (slot.state == onyx_slot::ONYX_SLOT_IDLE) {
                continue;
            }
            bool cancelled = false;
            {
                std::lock_guard<std::mutex> lock(slot.req->mu);
                cancelled = slot.req->cancelled;
            }
            if (cancelled) {
                finish(slot, ONYX_FINISH_CANCEL);
            }
        }

        try {
            admit_queued();
            build_batch(batch);
            if (batch.n_tokens == 0) {
                continue;
            }
            decode_batch(batch);
        } catch (const std::exception & e) {
            fprintf(stderr, "onyx-engine: engine tick failed: %s\n", e.what());
            llama_memory_clear(llama_get_memory(ctx_), true);
            for (auto & slot : slots_) {
                if (slot.state != onyx_slot::ONYX_SLOT_IDLE) {
                    slot.cache_tokens.clear();
                    slot.n_past = 0;
                    finish(slot, ONYX_FINISH_STOP, std::string("engine tick failed: ") + e.what());
                }
            }
        }
    }

    llama_batch_free(batch);

    // unblock anything still waiting on shutdown
    for (auto & slot : slots_) {
        if (slot.state != onyx_slot::ONYX_SLOT_IDLE) {
            finish(slot, ONYX_FINISH_CANCEL, "engine is shutting down");
        }
    }
    std::deque<onyx_gen_request_ptr> pending;
    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        pending.swap(queue_);
    }
    for (auto & req : pending) {
        req->smpl.reset();
        req->result.error = "engine is shutting down";
        {
            std::lock_guard<std::mutex> lock(req->mu);
            req->finished = true;
        }
        req->cv.notify_all();
    }
}

// Finishes every queued request whose caller gave up, and with
// --queue-timeout every one that has waited too long for a slot, wherever it
// sits in the queue. Only the engine thread removes requests, so erasing by
// identity is safe while HTTP threads keep appending.
void onyx_engine::sweep_queue() {
    std::vector<onyx_gen_request_ptr> waiting;
    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        if (queue_.empty()) {
            return;
        }
        waiting.assign(queue_.begin(), queue_.end());
    }

    const int64_t now = ggml_time_us();
    std::vector<onyx_gen_request_ptr> done;
    for (auto & req : waiting) {
        std::lock_guard<std::mutex> lock(req->mu);
        if (req->cancelled) {
            req->result.finish = ONYX_FINISH_CANCEL;
        } else if (queue_timeout_us_ > 0 && now - req->t_queued >= queue_timeout_us_) {
            req->result.error = "server is busy: no slot became free within " +
                                std::to_string(queue_timeout_us_ / 1000000) + " s (--queue-timeout)";
            req->result.queue_timeout = true;
        } else {
            continue;
        }
        done.push_back(req);
    }
    if (done.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(q_mutex_);
        for (const auto & req : done) {
            queue_.erase(std::remove(queue_.begin(), queue_.end(), req), queue_.end());
        }
    }
    for (auto & req : done) {
        if (req->result.queue_timeout) {
            std::lock_guard<std::mutex> m_lock(metrics_mutex_);
            metrics_.requests_timed_out++;
        }
        req->smpl.reset();
        {
            std::lock_guard<std::mutex> lock(req->mu);
            req->finished = true;
        }
        req->cv.notify_all();
    }
}

bool onyx_engine::admit_queued() {
    bool admitted = false;

    sweep_queue();

    while (true) {
        onyx_gen_request_ptr req;
        {
            std::lock_guard<std::mutex> lock(q_mutex_);
            if (queue_.empty()) {
                break;
            }
            req = queue_.front();
        }
        {
            std::lock_guard<std::mutex> lock(req->mu);
            if (req->cancelled) {
                std::lock_guard<std::mutex> q_lock(q_mutex_);
                queue_.pop_front();
                req->finished = true;
                req->result.finish = ONYX_FINISH_CANCEL;
                req->smpl.reset();
                req->cv.notify_all();
                continue;
            }
        }

        // pick the idle slot whose cached tokens share the longest prefix with
        // this prompt; ties (and the no-reuse case) fall back to LRU
        onyx_slot * best     = nullptr;
        size_t    best_lcp = 0;
        for (auto & slot : slots_) {
            if (slot.state != onyx_slot::ONYX_SLOT_IDLE) {
                continue;
            }
            size_t lcp = 0;
            // media prompts are never prefix-matched (see prefill_media)
            if (cache_reuse_ > 0 && req->params.cache_prompt && req->params.media.empty() &&
                seq_rm_ != ONYX_SEQ_RM_NONE) {
                const size_t max_lcp = std::min(slot.cache_tokens.size(), req->params.prompt_tokens.size() - 1);
                while (lcp < max_lcp && slot.cache_tokens[lcp] == req->params.prompt_tokens[lcp]) {
                    lcp++;
                }
                // memory that removes only whole sequences keeps a slot's
                // state only for a prompt that extends all of it, so a
                // partial match is worth nothing there (and would cost the
                // slot whose state could be extended)
                if (seq_rm_ == ONYX_SEQ_RM_FULL && lcp < slot.cache_tokens.size()) {
                    lcp = 0;
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

        onyx_slot & slot = *best;
        {
            std::lock_guard<std::mutex> m_lock(metrics_mutex_);
            metrics_.requests_processing++;
        }
        slot.req       = req;
        slot.prompt    = req->params.prompt_tokens;
        slot.has_media = !req->params.media.empty();
        slot.state     = onyx_slot::ONYX_SLOT_PREFILL;
        slot.i_batch   = -1;
        slot.n_sent    = 0;
        slot.t_start   = ggml_time_us();
        slot.t_prompt  = 0;

        slot.n_probs_sent        = 0;
        slot.probs_covered_bytes = 0;

        if (req->params.reasoning.enabled) {
            slot.rb_state = req->params.reasoning.start_in_prompt ? onyx_slot::RB_COUNTING : onyx_slot::RB_IDLE;
        } else {
            slot.rb_state = onyx_slot::RB_OFF;
        }
        slot.rb_count       = 0;
        slot.rb_forced_idx  = 0;
        slot.rb_start_match = 0;
        slot.rb_end_match   = 0;

        req->result.n_prompt = (int32_t) slot.prompt.size();

        // trim this sequence's KV back to the reusable prefix, leaving at
        // least one prompt token to evaluate so there are fresh logits
        llama_memory_t mem = llama_get_memory(ctx_);
        size_t prefix = 0;
        if (cache_reuse_ > 0 && req->params.cache_prompt && !slot.has_media && (int32_t) best_lcp >= cache_reuse_) {
            prefix = best_lcp;
        }
        if (prefix > 0) {
            // llama-server's test of whether the state can serve this prefix:
            // the first new token (at position `prefix`) attends to the
            // positions after prefix - n_swa, so a sliding-window cache that
            // still starts at or before prefix - n_swa + 1 is enough; any
            // other memory (recurrent state, or a pruned window) must not
            // reach past the prefix. Plain KV caches keep position 0.
            const llama_pos pos_min   = llama_memory_seq_pos_min(mem, slot.seq_id);
            const llama_pos pos_eval  = (llama_pos) prefix;
            const llama_pos pos_thold = std::max<llama_pos>(0, n_swa_ > 0 ? pos_eval - n_swa_ + 2 : pos_eval);
            if (pos_min < 0 || pos_min >= pos_thold ||
                !llama_memory_seq_rm(mem, slot.seq_id, (llama_pos) prefix, -1)) {
                llama_memory_seq_rm(mem, slot.seq_id, -1, -1);
                slot.cache_tokens.clear();
                prefix = 0;
            } else {
                slot.cache_tokens.resize(prefix);
            }
        } else {
            llama_memory_seq_rm(mem, slot.seq_id, -1, -1);
            slot.cache_tokens.clear();
        }
        req->result.n_cached = (int32_t) prefix;
        slot.n_past          = (int32_t) prefix;

        admitted = true;

        if (slot.has_media) {
            prefill_media(slot);
        }
    }

    return admitted;
}

void onyx_engine::build_batch(llama_batch & batch) {
    onyx_batch_clear(batch);
    for (auto & slot : slots_) {
        slot.i_batch = -1;
    }

    // exactly one next-token row per generating slot
    for (auto & slot : slots_) {
        if (slot.state != onyx_slot::ONYX_SLOT_GENERATE) {
            continue;
        }
        if (batch.n_tokens >= n_batch_) {
            break;
        }
        slot.i_batch = batch.n_tokens;
        onyx_batch_add(batch, slot.sampled, slot.n_past, { slot.seq_id }, true);
        if (!slot.has_media) {
            // media slots never reuse their KV, so the mirror array they would
            // be matched against is not maintained (and could not be: an image
            // chunk's positions have no per-token ids)
            slot.cache_tokens.push_back(slot.sampled);
        }
        slot.n_past++;
    }

    // fill the remainder with prompt chunks, split evenly across the slots
    // that are still prefilling
    int32_t n_prefilling = 0;
    for (const auto & slot : slots_) {
        if (slot.state == onyx_slot::ONYX_SLOT_PREFILL) {
            n_prefilling++;
        }
    }
    if (n_prefilling == 0) {
        return;
    }

    // --tick-tokens bounds the whole tick, decode rows included, so a burst
    // of long prompts cannot stretch every streaming slot's next token. The
    // prompt chunks always get at least one token, so prefill never starves.
    const int32_t room     = n_batch_ - batch.n_tokens;
    const int32_t budget   = tick_tokens_ > 0 ? std::min(room, std::max(1, tick_tokens_ - batch.n_tokens)) : room;
    const int32_t end      = batch.n_tokens + budget;
    const int32_t per_slot = std::max(1, budget / n_prefilling);

    for (auto & slot : slots_) {
        if (slot.state != onyx_slot::ONYX_SLOT_PREFILL) {
            continue;
        }
        const int32_t n_prompt = (int32_t) slot.prompt.size();
        const int32_t n        = std::min({ n_prompt - slot.n_past, per_slot, end - batch.n_tokens });
        for (int32_t k = 0; k < n; k++) {
            const bool last = slot.n_past == n_prompt - 1;
            if (last) {
                slot.i_batch = batch.n_tokens;
            }
            onyx_batch_add(batch, slot.prompt[(size_t) slot.n_past], slot.n_past, { slot.seq_id }, last);
            slot.cache_tokens.push_back(slot.prompt[(size_t) slot.n_past]);
            slot.n_past++;
        }
    }
}

bool onyx_engine::decode_batch(llama_batch & batch) {
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
                if (slot.state == onyx_slot::ONYX_SLOT_IDLE) {
                    continue;
                }
                llama_memory_seq_rm(mem, slot.seq_id, -1, -1);
                slot.cache_tokens.clear();
                slot.n_past = 0;
                finish(slot, ONYX_FINISH_STOP, err);
            }
            return false;
        }

        {
            int32_t n_busy = 0;
            for (const auto & slot : slots_) {
                n_busy += slot.state != onyx_slot::ONYX_SLOT_IDLE;
            }
            std::lock_guard<std::mutex> m_lock(metrics_mutex_);
            metrics_.n_decode++;
            metrics_.n_busy_slots += (uint64_t) n_busy;
        }

        // sample for every slot whose logits row landed in this view
        for (auto & slot : slots_) {
            if (slot.i_batch < off || slot.i_batch >= off + n) {
                continue;
            }
            const int32_t tok_idx = slot.i_batch - off;
            slot.i_batch = -1;

            try {
                sample_slot(slot, tok_idx);
            } catch (const std::exception & e) {
                finish(slot, ONYX_FINISH_STOP, std::string("generation failed: ") + e.what());
            }
        }

        off += n;
        n_view = n_batch_;
    }

    return true;
}

// Prefills a media-bearing request. mtmd splits the rendered prompt (which
// carries one media marker per buffer) plus the decoded buffers into
// text/image/audio chunks, and each chunk is evaluated on this slot's
// sequence.
//
// NOTE: this SERIALIZES the engine loop. mtmd_helper_eval_chunk_single runs
// the vision/audio encoder and issues its own llama_decode calls, and is
// documented as not thread-safe, so no other slot advances while a media
// prompt is prefilling. That is the trade upstream makes too (its own slot
// loop encodes media chunks inline); text-only requests are unaffected and
// keep the interleaved chunked-prefill path in build_batch().
bool onyx_engine::prefill_media(onyx_slot & slot) {
    onyx_gen_request & req = *slot.req;

    auto fail = [&](const std::string & err) {
        llama_memory_seq_rm(llama_get_memory(ctx_), slot.seq_id, -1, -1);
        slot.n_past = 0;
        slot.cache_tokens.clear();
        finish(slot, ONYX_FINISH_STOP, err);
        return false;
    };

    // MTMD_VIDEO is compiled out (see CMakeLists.txt), so the wrapper's
    // video_ctx is always null and only the bitmap needs freeing
    mtmd::bitmaps bitmaps;
    for (const auto & buf : req.params.media) {
        auto out = mtmd_helper_bitmap_init_from_buf(mctx_, buf.data(), buf.size(),
                                                    /* placeholder */ false,
                                                    mtmd_helper_init_opt_default());
        if (!out.bitmap) {
            return fail("failed to decode media input (unsupported or corrupt image/audio data)");
        }
        bitmaps.entries.emplace_back(out.bitmap);
    }

    mtmd_input_text text {
        /* text          */ req.params.prompt_text.c_str(),
        /* text_len      */ req.params.prompt_text.size(),
        /* add_special   */ true,
        /* parse_special */ true,
    };

    mtmd::input_chunks chunks(mtmd_input_chunks_init());
    auto bitmaps_c_ptr = bitmaps.c_ptr();
    const int32_t rc = mtmd_tokenize(mctx_, chunks.ptr.get(), &text, bitmaps_c_ptr.data(), bitmaps_c_ptr.size());
    if (rc == 1) {
        return fail("number of media inputs does not match the number of media markers in the prompt");
    }
    if (rc != 0) {
        return fail("failed to preprocess media input");
    }

    const llama_pos n_pos_total = mtmd_helper_get_n_pos(chunks.ptr.get());
    if (n_pos_total >= (llama_pos) n_ctx_slot_) {
        return fail("prompt (" + std::to_string(n_pos_total) + " positions, media included) does not fit in the "
                    "context window (" + std::to_string(n_ctx_slot_) + " tokens)");
    }

    // n_past must be tracked strictly from the out-param: with M-RoPE a
    // chunk's position count is not its token count
    llama_pos    n_past   = slot.n_past;   // always 0 here: media never reuses KV
    const size_t n_chunks = chunks.size();
    for (size_t i = 0; i < n_chunks; i++) {
        llama_pos new_n_past = n_past;
        const int32_t ret = mtmd_helper_eval_chunk_single(mctx_, ctx_, chunks[i], n_past, slot.seq_id, n_batch_,
                                                          /* logits_last */ i + 1 == n_chunks, &new_n_past);
        if (ret != 0) {
            return fail("failed to evaluate media prompt");
        }
        n_past = new_n_past;
    }

    slot.n_past          = (int32_t) n_past;
    req.result.n_prompt  = (int32_t) n_past;   // total positions, media included

    // the final chunk was evaluated with logits_last, so this slot's
    // next-token logits are the last row of that decode
    sample_slot(slot, -1);
    return true;
}

// Samples one token for `slot` from row `tok_idx` of the decode that just ran
// (-1 = the last row) and applies the v1 per-token bookkeeping.
void onyx_engine::sample_slot(onyx_slot & slot, int32_t tok_idx) {
    onyx_gen_request & req = *slot.req;
    onyx_gen_result  & res = req.result;

    if (slot.state == onyx_slot::ONYX_SLOT_PREFILL) {
        slot.state      = onyx_slot::ONYX_SLOT_GENERATE;
        slot.t_prompt   = ggml_time_us();
        res.t_prompt_ms = (slot.t_prompt - slot.t_start) / 1000.0;
    }

    // v1 ordering: budget checks first, then sample from the logits this
    // decode just produced
    if (req.params.n_predict >= 0 && res.n_predicted >= req.params.n_predict) {
        finish(slot, ONYX_FINISH_LENGTH);
        return;
    }
    if (slot.n_past >= (int32_t) n_ctx_slot_) {
        if (!ctx_shift_ || !context_shift(slot)) {
            finish(slot, ONYX_FINISH_LENGTH);
            return;
        }
    }

    // reasoning-budget (v2): own state machine, no llama_sampler involved.
    // Checked *before* sampling so budget==0 forces on the very first
    // generated token, and budget==N forces exactly after N counted tokens
    // (the token that would be the (N+1)th inside the block is forced
    // instead of sampled).
    const onyx_reasoning_budget & rb = req.params.reasoning;
    if (slot.rb_state == onyx_slot::RB_COUNTING && rb.budget >= 0 && slot.rb_count >= rb.budget) {
        slot.rb_state      = onyx_slot::RB_FORCING;
        slot.rb_forced_idx = 0;
    }

    llama_token tok;
    const bool forcing = slot.rb_state == onyx_slot::RB_FORCING && slot.rb_forced_idx < rb.forced.size();
    if (forcing) {
        // still "decode" the forced token through the sampler's accept path
        // so its penalty state stays consistent (the grammar does not see
        // forced tokens), but it is not sampled -- all other logits are
        // irrelevant, we already know what comes next.
        tok = rb.forced[slot.rb_forced_idx++];
        req.smpl->accept(tok, false);
        if (slot.rb_forced_idx >= rb.forced.size()) {
            slot.rb_state = onyx_slot::RB_DONE;
        }
    } else {
        try {
            tok = req.smpl->sample(ctx_, tok_idx);
            req.smpl->accept(tok, true);
        } catch (const std::exception & e) {
            // a grammar with no allowed token left fails this request only
            finish(slot, ONYX_FINISH_STOP, std::string("sampling failed: ") + e.what());
            return;
        }

        if (slot.rb_state == onyx_slot::RB_IDLE && !rb.start_tag.empty()) {
            // rolling match: a mismatch restarts the match window at 1 if
            // this token happens to also be the tag's first token
            if (tok == rb.start_tag[slot.rb_start_match]) {
                if (++slot.rb_start_match >= rb.start_tag.size()) {
                    slot.rb_state       = onyx_slot::RB_COUNTING;
                    slot.rb_count       = 0;
                    slot.rb_start_match = 0;
                    slot.rb_end_match   = 0;
                }
            } else {
                slot.rb_start_match = (tok == rb.start_tag[0]) ? 1 : 0;
            }
        } else if (slot.rb_state == onyx_slot::RB_COUNTING) {
            slot.rb_count++;
            if (!rb.end_tag.empty()) {
                if (tok == rb.end_tag[slot.rb_end_match]) {
                    if (++slot.rb_end_match >= rb.end_tag.size()) {
                        slot.rb_state = onyx_slot::RB_DONE;   // natural close
                    }
                } else {
                    slot.rb_end_match = (tok == rb.end_tag[0]) ? 1 : 0;
                }
            }
        }
    }

    res.n_predicted++;
    slot.sampled = tok;

    onyx_token_probs probs;
    const bool have_probs = req.params.want_logprobs;
    if (have_probs) {
        probs = compute_token_probs(ctx_, vocab_, tok_idx, tok, req.params.n_probs,
                                    req.params.sampling.preserved_tokens);
    }

    on_sampled(slot, have_probs ? &probs : nullptr);
}

// EOG / stop-sequence / stop-holdback handling for one freshly sampled token.
// Mirrors v1's per-token block, extended to keep `result.probs` (and what is
// delivered to the streaming callback) consistent with the trimmed text a
// client actually sees.
void onyx_engine::on_sampled(onyx_slot & slot, const onyx_token_probs * probs) {
    onyx_gen_request & req = *slot.req;
    onyx_gen_result  & res = req.result;

    if (llama_vocab_is_eog(vocab_, slot.sampled)) {
        finish(slot, ONYX_FINISH_STOP);
        return;
    }

    res.text += onyx_token_to_piece(vocab_, slot.sampled, req.params.sampling.preserved_tokens.count(slot.sampled) > 0);
    if (probs) {
        res.probs.push_back(*probs);
    }

    for (const auto & stop : req.params.stop) {
        const size_t pos = res.text.find(stop, slot.n_sent > stop.size() ? slot.n_sent - stop.size() : 0);
        if (pos != std::string::npos) {
            res.text.resize(pos);
            res.stopping_word = stop;
            if (!res.probs.empty()) {
                // drop any prob entry whose token text lands even partially
                // beyond the trimmed point, so probs stay 1:1 with the text
                // the client will see
                size_t cum  = 0;
                size_t keep = 0;
                for (; keep < res.probs.size(); keep++) {
                    cum += res.probs[keep].sampled.piece.size();
                    if (cum > pos) {
                        break;
                    }
                }
                res.probs.resize(keep);
            }
            finish(slot, ONYX_FINISH_STOP);
            return;
        }
    }

    if (req.wants_pieces) {
        const size_t hold = stop_holdback(res.text, req.params.stop);
        if (res.text.size() - hold > slot.n_sent) {
            std::string piece = res.text.substr(slot.n_sent, res.text.size() - hold - slot.n_sent);
            slot.n_sent += piece.size();
            emit(slot, std::move(piece), collect_delivered_probs(slot, res));
        }
    }
}

// Drops the oldest half of this slot's non-preserved context, exactly as
// llama-server does, so generation can continue past the context limit.
bool onyx_engine::context_shift(onyx_slot & slot) {
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
    if (!llama_memory_seq_rm(mem, slot.seq_id, n_keep, n_keep + n_discard)) {
        return false;
    }
    llama_memory_seq_add(mem, slot.seq_id, n_keep + n_discard, slot.n_past, -n_discard);

    for (size_t i = (size_t) (n_keep + n_discard); i < slot.cache_tokens.size(); i++) {
        slot.cache_tokens[i - (size_t) n_discard] = slot.cache_tokens[i];
    }
    slot.cache_tokens.resize(slot.cache_tokens.size() - (size_t) n_discard);
    slot.n_past -= n_discard;

    slot.req->result.truncated = true;
    return true;
}

void onyx_engine::emit(onyx_slot & slot, std::string piece, std::vector<onyx_token_probs> probs) {
    onyx_gen_request & req = *slot.req;
    {
        std::lock_guard<std::mutex> lock(req.mu);
        req.pieces.push_back(onyx_gen_piece{std::move(piece), std::move(probs)});
    }
    req.cv.notify_all();
}

void onyx_engine::finish(onyx_slot & slot, onyx_finish_reason reason, const std::string & error) {
    onyx_gen_request_ptr req = slot.req;
    onyx_gen_result &    res = req->result;

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

    {
        std::lock_guard<std::mutex> m_lock(metrics_mutex_);
        metrics_.prompt_tokens     += (uint64_t) std::max(0, res.n_prompt - res.n_cached);
        metrics_.prompt_seconds    += res.t_prompt_ms / 1000.0;
        metrics_.predicted_tokens  += (uint64_t) std::max(0, res.n_predicted);
        metrics_.predicted_seconds += res.t_predict_ms / 1000.0;
        metrics_.n_tokens_max       = std::max(metrics_.n_tokens_max, (uint64_t) std::max(0, slot.n_past));
        metrics_.requests_processing--;
    }

    // hand over any text withheld for stop-sequence matching, along with
    // whatever prob entries that final flush now fully covers
    if (req->wants_pieces && res.error.empty() && reason != ONYX_FINISH_CANCEL && res.text.size() > slot.n_sent) {
        const size_t old_sent = slot.n_sent;
        slot.n_sent = res.text.size();
        emit(slot, res.text.substr(old_sent), collect_delivered_probs(slot, res));
    }

    req->smpl.reset();

    slot.req.reset();
    slot.state       = onyx_slot::ONYX_SLOT_IDLE;
    slot.has_media   = false;
    slot.prompt.clear();
    slot.i_batch     = -1;
    slot.n_sent      = 0;
    slot.n_probs_sent        = 0;
    slot.probs_covered_bytes = 0;
    slot.rb_state    = onyx_slot::RB_OFF;
    slot.rb_count    = 0;
    slot.rb_forced_idx  = 0;
    slot.rb_start_match = 0;
    slot.rb_end_match   = 0;
    slot.t_prompt    = 0;
    slot.t_last_used = ++tick_;

    {
        std::lock_guard<std::mutex> lock(req->mu);
        req->finished = true;
    }
    req->cv.notify_all();
}

// ---------------------------------------------------------------------------
// decision mode
// ---------------------------------------------------------------------------

// Keeps the longest prefix of seq 0 that `tokens` starts with, up to n_max
// tokens, and drops the rest of seq 0. Returns how many tokens are kept.
int32_t onyx_engine::decision_reuse(const std::vector<llama_token> & tokens, int32_t n_max) {
    llama_memory_t mem = llama_get_memory(ctx_);

    size_t n_keep = 0;
    if (mem && decision_reuse_on_) {
        const size_t n_lim = (size_t) std::max(0, n_max);
        while (n_keep < decision_cache_.size() && n_keep < tokens.size() && n_keep < n_lim &&
               decision_cache_[n_keep] == tokens[n_keep]) {
            n_keep++;
        }
    }
    if (mem && n_keep == 0) {
        llama_memory_seq_rm(mem, 0, -1, -1);
    } else if (mem && n_keep < decision_cache_.size() && !llama_memory_seq_rm(mem, 0, (llama_pos) n_keep, -1)) {
        // recurrent state cannot be cut back; start over then
        llama_memory_seq_rm(mem, 0, -1, -1);
        n_keep = 0;
    }
    decision_cache_.resize(n_keep);
    return (int32_t) n_keep;
}

// Decodes p.tokens[from, end) into `seq` at their own positions and reads
// p's outputs. The outputs sit in one batch: everything before the first
// position read goes in n_batch chunks, the rest in one decode. With `out`
// null the tokens are only evaluated (a shared prefix).
bool onyx_engine::decision_eval(const onyx_decision_prompt & p, llama_seq_id seq, int32_t from,
                                std::vector<float> * out, std::string & err) {
    const int32_t n = (int32_t) p.tokens.size();
    const bool    want_out = out != nullptr;

    // the first position whose output is read: every one from there is an
    // output when embeddings are read, only the last otherwise
    int32_t out_from = n;
    if (want_out) {
        const int32_t first = p.pos_first();
        out_from = std::max(from, first >= 0 ? first : n - 1);
        if (!llama_get_causal_attn(ctx_)) {
            out_from = from; // non-causal: the whole prompt sees itself in one batch
        }
        if (n - out_from > n_batch_) {
            err = "prompt: the question and its options (" + std::to_string(n - out_from) +
                  " tokens) do not fit in one batch; raise --batch-size (now " + std::to_string(n_batch_) + ")";
            return false;
        }
    }

    llama_batch_ext_ptr batch(llama_batch_ext_init(ctx_));
    for (int32_t pos = from; pos < n; ) {
        const int32_t end = pos < out_from ? std::min(out_from, pos + n_batch_) : n;
        llama_batch_ext_clear(batch.get());
        for (int32_t i = pos; i < end; i++) {
            const int32_t   idx = llama_batch_ext_add_token(batch.get(), seq, p.tokens[(size_t) i]);
            const llama_pos ipos = i;
            if (idx < 0 || !llama_batch_ext_set_pos(batch.get(), idx, &ipos)) {
                err = "failed to build the decision batch";
                return false;
            }
            if (i >= out_from && (p.reads_embeddings() || i == n - 1)) {
                llama_batch_ext_set_output_logits(batch.get(), idx, true);
            }
            if (!p.order.empty() && p.order[(size_t) i] != LLAMA_DECISION_ORDER_NONE) {
                llama_batch_ext_set_decision_order(batch.get(), idx, (llama_decision_order) p.order[(size_t) i]);
            }
        }
        const int32_t rc = llama_process(ctx_, LLAMA_PROCESS_TYPE_DECODE, batch.get());
        if (rc != 0) {
            err = rc == 1 ? "prompt does not fit in the context window (no free KV slot)" : "decision evaluation failed";
            return false;
        }
        pos = end;
    }
    if (!want_out) {
        return true;
    }

    // outputs of the last batch: batch index == position - out_from
    std::vector<float> & scores = *out;
    scores.clear();
    if (!p.labels.empty()) {
        const float * logits = llama_get_logits_ith(ctx_, n - 1 - out_from);
        if (!logits) {
            err = "the model returned no logits";
            return false;
        }
        for (const llama_token label : p.labels) {
            scores.push_back(logits[label]);
        }
        if (!p.label_groups.empty()) {
            std::vector<float> grouped;
            size_t i = 0;
            for (const int32_t k : p.label_groups) {
                grouped.push_back(*std::max_element(scores.begin() + i, scores.begin() + i + k));
                i += (size_t) k;
            }
            scores = std::move(grouped);
        }
        return true;
    }

    auto embd_at = [&](int32_t pos) -> const float * {
        return pos >= out_from && pos < n ? llama_get_embeddings_ith(ctx_, pos - out_from) : nullptr;
    };

    // joint head: the scores are the first rows
    for (int32_t i = 0; i < p.n_scores; i++) {
        const float * embd = i < n - out_from ? llama_get_embeddings_ith(ctx_, i) : nullptr;
        if (!embd) {
            err = "the model returned no embeddings";
            return false;
        }
        scores.push_back(embd[0]);
    }

    const int32_t n_embd_out = llama_model_n_embd_out(model_);
    const int32_t n_half     = n_embd_out / 2;
    const float * embd_q     = p.pointer >= 0 ? embd_at(p.pointer) : nullptr;
    if (p.column < 0 || p.column >= n_embd_out) {
        err = "the decision output column is out of range";
        return false;
    }
    for (const int32_t marker : p.markers) {
        const float * embd = embd_at(marker);
        if (!embd || (p.pointer >= 0 && !embd_q)) {
            err = "the model returned no embeddings";
            return false;
        }
        if (p.pointer < 0) {
            scores.push_back(embd[p.column]);
            continue;
        }
        // kev: embeddings are [q | k]
        float dot = 0.0f;
        for (int32_t i = 0; i < n_half; i++) {
            dot += embd_q[i] * embd[n_half + i];
        }
        scores.push_back(dot / std::sqrt((float) n_half));
    }
    return true;
}

// A prompt with media, label families only: prefilled through mtmd into a
// cleared seq 0, the labels read from the last row.
bool onyx_engine::decision_eval_media(const onyx_decision_prompt & p, std::vector<float> & scores,
                                      int32_t & n_tokens, std::string & err) {
    if (!mctx_ || p.labels.empty()) {
        err = "prompt: image input is not supported for this decision model";
        return false;
    }
    decision_reuse(p.tokens, 0);

    mtmd::bitmaps bitmaps;
    for (const auto & buf : p.media) {
        auto out = mtmd_helper_bitmap_init_from_buf(mctx_, buf.data(), buf.size(), /* placeholder */ false,
                                                    mtmd_helper_init_opt_default());
        if (!out.bitmap) {
            err = "prompt: failed to decode media input (unsupported or corrupt image/audio data)";
            return false;
        }
        bitmaps.entries.emplace_back(out.bitmap);
    }

    mtmd_input_text text {
        /* text          */ p.text.c_str(),
        /* text_len      */ p.text.size(),
        /* add_special   */ false,
        /* parse_special */ true,
    };
    mtmd::input_chunks chunks(mtmd_input_chunks_init());
    auto bitmaps_c_ptr = bitmaps.c_ptr();
    const int32_t rc = mtmd_tokenize(mctx_, chunks.ptr.get(), &text, bitmaps_c_ptr.data(), bitmaps_c_ptr.size());
    if (rc != 0) {
        err = rc == 1 ? "prompt: the number of images does not match the template's media markers"
                      : "prompt: failed to preprocess media input";
        return false;
    }
    const llama_pos n_pos_total = mtmd_helper_get_n_pos(chunks.ptr.get());
    if (n_pos_total >= (llama_pos) n_ctx_slot_) {
        err = "prompt (" + std::to_string(n_pos_total) + " positions, media included) does not fit in the "
              "context window (" + std::to_string(n_ctx_slot_) + " tokens)";
        return false;
    }

    llama_pos n_past = 0;
    const size_t n_chunks = chunks.size();
    for (size_t i = 0; i < n_chunks; i++) {
        llama_pos new_n_past = n_past;
        if (mtmd_helper_eval_chunk_single(mctx_, ctx_, chunks[i], n_past, 0, n_batch_,
                                          /* logits_last */ i + 1 == n_chunks, &new_n_past) != 0) {
            llama_memory_seq_rm(llama_get_memory(ctx_), 0, -1, -1);
            err = "failed to evaluate media prompt";
            return false;
        }
        n_past = new_n_past;
    }
    // media positions cannot be prefix-matched, so nothing is kept
    llama_memory_seq_rm(llama_get_memory(ctx_), 0, -1, -1);

    const float * logits = llama_get_logits_ith(ctx_, -1);
    if (!logits) {
        err = "the model returned no logits";
        return false;
    }
    scores.clear();
    for (const llama_token label : p.labels) {
        scores.push_back(logits[label]);
    }
    if (!p.label_groups.empty()) {
        std::vector<float> grouped;
        size_t i = 0;
        for (const int32_t k : p.label_groups) {
            grouped.push_back(*std::max_element(scores.begin() + i, scores.begin() + i + k));
            i += (size_t) k;
        }
        scores = std::move(grouped);
    }
    n_tokens += (int32_t) mtmd_helper_get_n_tokens(chunks.ptr.get());
    return true;
}

bool onyx_engine::decide(const std::vector<onyx_decision_prompt> & prompts, bool share_prefix,
                         std::vector<std::vector<float>> & scores, int32_t & n_tokens, std::string & err) {
    std::lock_guard<std::mutex> lock(mutex_);

    scores.assign(prompts.size(), {});
    n_tokens = 0;
    if (!decision_mode()) {
        err = "this instance is not serving a decision model";
        return false;
    }

    const int32_t n_vocab = llama_vocab_n_tokens(vocab_);
    bool has_media = false;
    for (const auto & p : prompts) {
        has_media = has_media || !p.media.empty();
        if (!p.media.empty()) {
            continue;
        }
        if (p.tokens.empty()) {
            err = "prompt produced no tokens";
            return false;
        }
        if (p.tokens.size() >= n_ctx_slot_) {
            err = "prompt (" + std::to_string(p.tokens.size()) + " tokens) does not fit in the context window (" +
                  std::to_string(n_ctx_slot_) + " tokens)";
            return false;
        }
        for (const llama_token t : p.labels) {
            if (t < 0 || t >= n_vocab) {
                err = "decision label is not a valid token";
                return false;
            }
        }
    }

    llama_memory_t mem    = llama_get_memory(ctx_);
    const bool     causal = llama_get_causal_attn(ctx_);
    // a prompt can continue from cached tokens only in a causal context with
    // memory, for a family whose prompts are read that way
    const bool can_continue = mem && causal && decision_.can_share_prompt();

    // the prefix all prompts share, evaluated once into seq 0; each prompt
    // then runs in seq 1 from a copy of it
    int32_t n_shared = 0;
    if (share_prefix && can_continue && prompts.size() > 1 && !has_media) {
        const auto & first = prompts[0].tokens;
        n_shared = (int32_t) first.size();
        for (const auto & p : prompts) {
            int32_t k = 0;
            while (k < n_shared && (size_t) k < p.tokens.size() && p.tokens[(size_t) k] == first[(size_t) k]) {
                k++;
            }
            // every prompt keeps at least one token, and everything it reads, for itself
            const int32_t first_read = p.pos_first();
            k = std::min(k, (int32_t) p.tokens.size() - 1);
            if (first_read >= 0) {
                k = std::min(k, first_read);
            }
            n_shared = std::min(n_shared, k);
        }
    }

    if (n_shared > 0) {
        const auto & first = prompts[0];
        const int32_t n_keep = decision_reuse(first.tokens, n_shared);
        onyx_decision_prompt prefix;
        prefix.tokens.assign(first.tokens.begin(), first.tokens.begin() + n_shared);
        if (n_keep < n_shared && !decision_eval(prefix, 0, n_keep, nullptr, err)) {
            decision_reuse({}, 0);
            return false;
        }
        decision_cache_ = prefix.tokens;

        for (size_t i = 0; i < prompts.size(); i++) {
            llama_memory_seq_rm(mem, 1, -1, -1);
            llama_memory_seq_cp(mem, 0, 1, -1, -1);
            if (!decision_eval(prompts[i], 1, n_shared, &scores[i], err)) {
                llama_memory_seq_rm(mem, 1, -1, -1);
                return false;
            }
            n_tokens += (int32_t) prompts[i].tokens.size();
        }
        llama_memory_seq_rm(mem, 1, -1, -1);
        return true;
    }

    for (size_t i = 0; i < prompts.size(); i++) {
        const auto & p = prompts[i];
        if (!p.media.empty()) {
            if (!decision_eval_media(p, scores[i], n_tokens, err)) {
                return false;
            }
            continue;
        }
        // reuse only what comes before the first position read, and never
        // the whole prompt
        int32_t n_max = 0;
        if (can_continue) {
            const int32_t first_read = p.pos_first();
            n_max = first_read >= 0 ? first_read : (int32_t) p.tokens.size() - 1;
        }
        const int32_t n_keep = decision_reuse(p.tokens, n_max);
        if (!decision_eval(p, 0, n_keep, &scores[i], err)) {
            decision_reuse({}, 0);
            return false;
        }
        if (can_continue) {
            decision_cache_ = p.tokens;
        } else if (mem) {
            llama_memory_seq_rm(mem, 0, -1, -1);
        }
        n_tokens += (int32_t) p.tokens.size();
    }
    return true;
}
