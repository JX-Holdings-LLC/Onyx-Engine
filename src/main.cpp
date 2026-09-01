#include "args.h"
#include "convert.h"
#include "engine.h"
#include "server.h"

#include "llama.h"
#include "log.h"

#include <cinttypes>
#include <cstdio>

int main(int argc, char ** argv) {
    jx_args args;
    if (!jx_args_parse(argc, argv, args)) {
        return 1;
    }
    if (args.show_help) {
        jx_args_print_help();
        return 0;
    }
    if (args.show_version) {
        jx_args_print_version();
        return 0;
    }

    if (!args.verbose) {
        // keep stderr limited to warnings/errors so managed-process logs stay readable
        llama_log_set([](ggml_log_level level, const char * text, void *) {
            if (level >= GGML_LOG_LEVEL_WARN) {
                fputs(text, stderr);
            }
        }, nullptr);
        common_log_set_verbosity_thold(-1);
    }

    {
        // safetensors models are converted to GGUF (cached) before loading
        std::string resolve_err;
        const std::string resolved = jx_resolve_model(args, resolve_err);
        if (resolved.empty()) {
            fprintf(stderr, "error: %s\n", resolve_err.c_str());
            return 1;
        }
        args.model_path = resolved;
    }

    llama_backend_init();

    fprintf(stderr, "jx-engine %s: loading model '%s'\n", JX_ENGINE_VERSION, args.model_path.c_str());

    jx_engine engine;
    if (!engine.load(args)) {
        fprintf(stderr, "error: %s\n", engine.load_error().c_str());
        llama_backend_free();
        return 1;
    }

    if (engine.n_parallel() > 1) {
        fprintf(stderr, "jx-engine: model loaded (%s, %" PRIu64 " MiB, %d slots x %u ctx%s)\n",
                engine.model_desc().c_str(), engine.model_size_bytes() / (1024 * 1024),
                engine.n_parallel(), engine.n_ctx_slot(),
                engine.ctx_shift() ? ", context shift" : "");
    } else {
        fprintf(stderr, "jx-engine: model loaded (%s, %" PRIu64 " MiB%s)\n",
                engine.model_desc().c_str(), engine.model_size_bytes() / (1024 * 1024),
                engine.ctx_shift() ? ", context shift" : "");
    }

    const int rc = jx_server_run(engine, args);

    llama_backend_free();
    return rc;
}
