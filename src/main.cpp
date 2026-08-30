#include "args.h"
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

    llama_backend_init();

    fprintf(stderr, "jx-engine %s: loading model '%s'\n", JX_ENGINE_VERSION, args.model_path.c_str());

    jx_engine engine;
    if (!engine.load(args)) {
        fprintf(stderr, "error: %s\n", engine.load_error().c_str());
        llama_backend_free();
        return 1;
    }

    fprintf(stderr, "jx-engine: model loaded (%s, %" PRIu64 " MiB)\n",
            engine.model_desc().c_str(), engine.model_size_bytes() / (1024 * 1024));

    const int rc = jx_server_run(engine, args);

    llama_backend_free();
    return rc;
}
