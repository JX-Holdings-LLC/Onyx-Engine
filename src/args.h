// jx-engine command line arguments.
//
// Flag names deliberately mirror llama-server's flags for the subset we
// implement, because JX Runtime's llamacpp adapter probes `--help` text to
// decide which flags a server binary understands. Flags we do not implement
// (e.g. --mmproj, --reasoning-budget) are intentionally absent from both the
// parser and the help text so callers never pass them expecting behavior we
// do not have.
#pragma once

#include <cstdint>
#include <string>

struct jx_args {
    // model
    std::string model_path;
    std::string alias;               // reported as the OpenAI "model" id
    std::string chat_template;       // named built-in template override
    std::string chat_template_file;  // template source file override

    // network
    std::string host = "127.0.0.1";
    int         port = 8080;
    std::string api_key;             // optional bearer token

    // context / compute
    int32_t n_ctx           = 4096;  // 0 = use model's training context
    int32_t n_batch         = 2048;
    int32_t n_ubatch        = 512;
    int32_t n_gpu_layers    = -1;    // -1 = offload all layers if possible
    int32_t n_threads       = -1;    // -1 = auto
    int32_t n_threads_batch = -1;    // -1 = same as n_threads
    int32_t n_parallel      = 1;     // accepted for compat; requests are queued
    int32_t flash_attn      = -1;    // -1 auto, 0 off, 1 on
    bool    mlock           = false;
    bool    no_mmap         = false;
    uint32_t seed           = 0xFFFFFFFF; // LLAMA_DEFAULT_SEED

    // features
    bool        embedding    = false; // enable embeddings endpoint (pooled)
    std::string pooling      = "";    // "", none, mean, cls, last, rank
    int32_t     cache_reuse  = 1;     // min prefix length to reuse KV cache; 0 disables
    bool        jinja        = true;  // accepted for compat; jinja is always used
    bool        verbose      = false;

    // actions
    bool show_help    = false;
    bool show_version = false;
};

// Parses argv. Returns false (after printing an error to stderr) on invalid
// input. On --help/--version, sets the corresponding flag and returns true.
bool jx_args_parse(int argc, char ** argv, jx_args & out);

void jx_args_print_help();
void jx_args_print_version();
