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

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = args.n_gpu_layers < 0 ? 999 : args.n_gpu_layers;
    mparams.load_mode    = pick_load_mode(args.no_mmap, args.mlock);

    model_ = llama_model_load_from_file(args.model_path.c_str(), mparams);
    if (!model_) {
        load_error_ = "failed to load model from '" + args.model_path + "'";
        return false;
    }
    vocab_ = llama_model_get_vocab(model_);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = args.n_ctx < 0 ? 0 : (uint32_t) args.n_ctx;
    cparams.n_batch   = (uint32_t) std::max<int32_t>(32, args.n_batch);
    cparams.n_ubatch  = (uint32_t) std::max<int32_t>(32, args.n_ubatch);
    cparams.n_seq_max = 1;

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
    n_ctx_   = llama_n_ctx(ctx_);
    n_batch_ = (int32_t) cparams.n_batch;

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

bool jx_engine::decode_prompt(const std::vector<llama_token> & tokens, int32_t & n_cached, std::string & err) {
    llama_memory_t mem = llama_get_memory(ctx_);

    // reuse the longest common prefix already in the KV cache, leaving at
    // least one prompt token to evaluate so we have fresh logits to sample from
    size_t prefix = 0;
    if (cache_reuse_ > 0 && !cache_tokens_.empty()) {
        const size_t max_prefix = std::min(cache_tokens_.size(), tokens.size() - 1);
        while (prefix < max_prefix && cache_tokens_[prefix] == tokens[prefix]) {
            prefix++;
        }
        if ((int32_t) prefix < cache_reuse_) {
            prefix = 0;
        }
    }

    if (prefix > 0) {
        llama_memory_seq_rm(mem, 0, (llama_pos) prefix, -1);
        cache_tokens_.resize(prefix);
    } else {
        llama_memory_clear(mem, true);
        cache_tokens_.clear();
    }
    n_cached = (int32_t) prefix;

    llama_batch batch = llama_batch_init(n_batch_, 0, 1);
    bool ok = true;
    for (size_t off = prefix; off < tokens.size(); ) {
        const size_t n = std::min((size_t) n_batch_, tokens.size() - off);
        common_batch_clear(batch);
        for (size_t k = 0; k < n; k++) {
            const bool last = off + k == tokens.size() - 1;
            common_batch_add(batch, tokens[off + k], (llama_pos) (off + k), {0}, last);
        }
        if (llama_decode(ctx_, batch) != 0) {
            err = "prompt evaluation failed (llama_decode) at token " + std::to_string(off);
            ok = false;
            break;
        }
        off += n;
    }
    llama_batch_free(batch);

    if (ok) {
        cache_tokens_.assign(tokens.begin(), tokens.end());
    } else {
        llama_memory_clear(mem, true);
        cache_tokens_.clear();
    }
    return ok;
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

jx_gen_result jx_engine::generate(const jx_gen_params & params, const jx_token_cb & cb) {
    std::lock_guard<std::mutex> lock(mutex_);

    jx_gen_result res;

    if (embedding_mode_) {
        res.error = "this instance is running in embedding mode; text generation is disabled";
        return res;
    }

    std::vector<llama_token> prompt = params.prompt_tokens;
    if (prompt.empty()) {
        // llama_decode needs at least one token; fall back to BOS
        const llama_token bos = llama_vocab_bos(vocab_);
        prompt.push_back(bos >= 0 ? bos : 0);
    }
    if (prompt.size() >= n_ctx_) {
        res.error = "prompt (" + std::to_string(prompt.size()) + " tokens) does not fit in the context window (" +
                    std::to_string(n_ctx_) + " tokens)";
        return res;
    }

    common_params_sampling sparams = params.sampling;
    common_sampler * smpl = nullptr;
    try {
        smpl = common_sampler_init(model_, sparams);
    } catch (const std::exception & e) {
        res.error = std::string("failed to initialize sampler: ") + e.what();
        return res;
    }
    if (!smpl) {
        res.error = "failed to initialize sampler (bad grammar?)";
        return res;
    }

    const int64_t t_start = ggml_time_us();

    if (!params.cache_prompt) {
        llama_memory_clear(llama_get_memory(ctx_), true);
        cache_tokens_.clear();
    }

    std::string err;
    if (!decode_prompt(prompt, res.n_cached, err)) {
        common_sampler_free(smpl);
        res.error = err;
        return res;
    }
    res.n_prompt = (int32_t) prompt.size();

    const int64_t t_prompt_done = ggml_time_us();
    res.t_prompt_ms = (t_prompt_done - t_start) / 1000.0;

    int32_t n_past = (int32_t) prompt.size();
    size_t  n_sent = 0;   // bytes of res.text already delivered to cb

    llama_batch batch = llama_batch_init(1, 0, 1);

    while (true) {
        if (params.n_predict >= 0 && res.n_predicted >= params.n_predict) {
            res.finish = JX_FINISH_LENGTH;
            break;
        }
        if (n_past >= (int32_t) n_ctx_) {
            res.finish = JX_FINISH_LENGTH;
            break;
        }

        const llama_token tok = common_sampler_sample(smpl, ctx_, -1);
        common_sampler_accept(smpl, tok, true);
        res.n_predicted++;

        if (llama_vocab_is_eog(vocab_, tok)) {
            res.finish = JX_FINISH_STOP;
            break;
        }

        res.text += common_token_to_piece(ctx_, tok);

        // stop-sequence scan over the accumulated tail
        bool stopped = false;
        for (const auto & stop : params.stop) {
            const size_t pos = res.text.find(stop, n_sent > stop.size() ? n_sent - stop.size() : 0);
            if (pos != std::string::npos) {
                res.text.resize(pos);
                res.stopping_word = stop;
                stopped = true;
                break;
            }
        }
        if (stopped) {
            res.finish = JX_FINISH_STOP;
            break;
        }

        if (cb) {
            const size_t hold = stop_holdback(res.text, params.stop);
            if (res.text.size() - hold > n_sent) {
                const std::string piece = res.text.substr(n_sent, res.text.size() - hold - n_sent);
                n_sent += piece.size();
                if (!cb(piece)) {
                    res.finish = JX_FINISH_CANCEL;
                    break;
                }
            }
        }

        common_batch_clear(batch);
        common_batch_add(batch, tok, n_past, {0}, true);
        if (llama_decode(ctx_, batch) != 0) {
            res.error = "token evaluation failed (llama_decode) at position " + std::to_string(n_past);
            break;
        }
        n_past++;
        cache_tokens_.push_back(tok);
    }

    llama_batch_free(batch);

    // deliver any withheld text
    if (cb && res.error.empty() && res.finish != JX_FINISH_CANCEL && res.text.size() > n_sent) {
        cb(res.text.substr(n_sent));
    }

    res.t_predict_ms = (ggml_time_us() - t_prompt_done) / 1000.0;

    common_sampler_free(smpl);
    return res;
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
    cache_tokens_.clear();

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
