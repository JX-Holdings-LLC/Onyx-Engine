#include "args.h"

#include "build-info.h"
#include "llama.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool parse_int(const char * s, int32_t & out) {
    char * end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        fprintf(stderr, "error: invalid integer value '%s'\n", s);
        return false;
    }
    out = (int32_t) v;
    return true;
}

void jx_args_print_version() {
    printf("jx-engine %s\n", JX_ENGINE_VERSION);
    printf("%s\n", llama_build_info());
}

void jx_args_print_help() {
    printf(
        "usage: jx-engine [options]\n"
        "\n"
        "JX Engine - OpenAI-compatible model serving binary for JX Runtime.\n"
        "Serves a single GGUF model per process.\n"
        "\n"
        "model:\n"
        "  -m,  --model PATH            path to the GGUF model file (required)\n"
        "  -a,  --alias NAME            model id reported by the API (default: file stem)\n"
        "       --chat-template NAME    override the model's chat template with a built-in one\n"
        "       --chat-template-file F  override the model's chat template from a file\n"
        "       --jinja                 apply the model's jinja chat template (always on)\n"
        "\n"
        "network:\n"
        "       --host HOST             address to bind (default: 127.0.0.1)\n"
        "       --port PORT             port to listen on (default: 8080)\n"
        "       --api-key KEY           require this bearer token on /v1 endpoints\n"
        "\n"
        "compute:\n"
        "  -c,  --ctx-size N            context size in tokens, 0 = model default (default: 4096)\n"
        "  -b,  --batch-size N          logical batch size (default: 2048)\n"
        "  -ub, --ubatch-size N         physical batch size (default: 512)\n"
        "  -ngl, --n-gpu-layers N       layers to offload to GPU, -1 = all (default: -1)\n"
        "  -t,  --threads N             generation threads, -1 = auto (default: -1)\n"
        "  -tb, --threads-batch N       prompt processing threads, -1 = same as --threads\n"
        "  -np, --parallel N            request slots; requests beyond this queue (default: 1)\n"
        "  -fa, --flash-attn VAL        flash attention: on, off, auto (default: auto)\n"
        "       --mlock                 lock model memory in RAM\n"
        "       --no-mmap               do not memory-map the model file\n"
        "  -s,  --seed N                default RNG seed (default: random)\n"
        "\n"
        "features:\n"
        "       --embedding             enable pooled embeddings (/v1/embeddings)\n"
        "       --embeddings            alias of --embedding\n"
        "       --pooling TYPE          pooling: none, mean, cls, last, rank (default: model)\n"
        "       --cache-reuse N         min prefix tokens to reuse from KV cache, 0 = off (default: 1)\n"
        "  -v,  --verbose               verbose logging\n"
        "\n"
        "misc:\n"
        "  -h,  --help                  show this help and exit\n"
        "       --version               show version and exit\n");
}

bool jx_args_parse(int argc, char ** argv, jx_args & out) {
    auto need_value = [&](int & i, const char * flag) -> const char * {
        if (i + 1 >= argc) {
            fprintf(stderr, "error: %s requires a value\n", flag);
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];

        auto is = [&](const char * a, const char * b = nullptr, const char * c = nullptr) {
            return arg == a || (b && arg == b) || (c && arg == c);
        };

        if (is("-h", "--help")) {
            out.show_help = true;
            return true;
        }
        if (is("--version")) {
            out.show_version = true;
            return true;
        }
        if (is("-v", "--verbose")) { out.verbose = true; continue; }
        if (is("--jinja"))         { out.jinja   = true; continue; }
        if (is("--mlock"))         { out.mlock   = true; continue; }
        if (is("--no-mmap"))       { out.no_mmap = true; continue; }
        if (is("--embedding", "--embeddings")) { out.embedding = true; continue; }

        const char * val = nullptr;

        if (is("-m", "--model"))              { if (!(val = need_value(i, argv[i]))) return false; out.model_path = val; continue; }
        if (is("-a", "--alias"))              { if (!(val = need_value(i, argv[i]))) return false; out.alias = val; continue; }
        if (is("--chat-template"))            { if (!(val = need_value(i, argv[i]))) return false; out.chat_template = val; continue; }
        if (is("--chat-template-file"))       { if (!(val = need_value(i, argv[i]))) return false; out.chat_template_file = val; continue; }
        if (is("--host"))                     { if (!(val = need_value(i, argv[i]))) return false; out.host = val; continue; }
        if (is("--api-key"))                  { if (!(val = need_value(i, argv[i]))) return false; out.api_key = val; continue; }
        if (is("--pooling"))                  { if (!(val = need_value(i, argv[i]))) return false; out.pooling = val; continue; }

        if (is("-fa", "--flash-attn")) {
            if (!(val = need_value(i, argv[i]))) return false;
            if      (strcmp(val, "on")   == 0 || strcmp(val, "1") == 0) out.flash_attn = 1;
            else if (strcmp(val, "off")  == 0 || strcmp(val, "0") == 0) out.flash_attn = 0;
            else if (strcmp(val, "auto") == 0)                          out.flash_attn = -1;
            else {
                fprintf(stderr, "error: invalid --flash-attn value '%s' (expected on|off|auto)\n", val);
                return false;
            }
            continue;
        }

        int32_t n = 0;
        if (is("--port"))                     { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.port = n; continue; }
        if (is("-c", "--ctx-size"))           { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_ctx = n; continue; }
        if (is("-b", "--batch-size"))         { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_batch = n; continue; }
        if (is("-ub", "--ubatch-size"))       { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_ubatch = n; continue; }
        if (is("-ngl", "--n-gpu-layers", "--gpu-layers")) { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_gpu_layers = n; continue; }
        if (is("-t", "--threads"))            { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_threads = n; continue; }
        if (is("-tb", "--threads-batch"))     { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_threads_batch = n; continue; }
        if (is("-np", "--parallel"))          { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_parallel = n; continue; }
        if (is("--cache-reuse"))              { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.cache_reuse = n; continue; }
        if (is("-s", "--seed"))               { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.seed = (uint32_t) n; continue; }

        fprintf(stderr, "error: unknown argument '%s' (see --help)\n", arg.c_str());
        return false;
    }

    if (out.model_path.empty()) {
        fprintf(stderr, "error: --model is required (see --help)\n");
        return false;
    }

    if (out.alias.empty()) {
        // default alias: model file name without directory or .gguf extension
        std::string base = out.model_path;
        const size_t slash = base.find_last_of("/\\");
        if (slash != std::string::npos) {
            base = base.substr(slash + 1);
        }
        const size_t dot = base.rfind(".gguf");
        if (dot != std::string::npos) {
            base = base.substr(0, dot);
        }
        out.alias = base.empty() ? "model" : base;
    }

    return true;
}
