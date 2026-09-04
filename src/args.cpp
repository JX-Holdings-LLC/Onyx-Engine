#include "args.h"

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

void onyx_args_print_version() {
    printf("onyx-engine %s\n", ONYX_ENGINE_VERSION);
    printf("llama.cpp %s\n", ONYX_ENGINE_LLAMA_PIN);
}

void onyx_args_print_help() {
    printf(
        "usage: onyx-engine [options]\n"
        "\n"
        "Onyx Engine - OpenAI-compatible model serving binary for JX Runtime.\n"
        "Serves a single GGUF model per process.\n"
        "\n"
        "model:\n"
        "  -m,  --model PATH            model to serve (required): a GGUF file, or a\n"
        "                               safetensors model (HF directory or .safetensors\n"
        "                               file) converted to GGUF on first load\n"
        "       --mmproj PATH           multimodal projector GGUF (enables image input)\n"
        "       --convert-dir DIR       cache directory for converted safetensors models\n"
        "                               (default: alongside the source model)\n"
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
        "  -np, --parallel N            concurrent request slots; requests beyond this\n"
        "                               queue (default: 1)\n"
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
        "       --context-shift         drop oldest tokens instead of stopping when the\n"
        "                               context fills mid-generation (default: off)\n"
        "       --no-context-shift      disable context shift (the default)\n"
        "       --keep N                tokens at the front preserved by a context shift,\n"
        "                               -1 = whole prompt (default: 0)\n"
        "       --reasoning-budget N    thinking-token budget: -1 = unrestricted,\n"
        "                               0 = suppress thinking, N > 0 = force the end of\n"
        "                               thinking after N tokens (default: -1)\n"
        "       --reasoning-budget-message MSG\n"
        "                               text injected before the forced end-of-thinking\n"
        "                               tag when the budget runs out\n"
        "  -v,  --verbose               verbose logging\n"
        "\n"
        "misc:\n"
        "  -h,  --help                  show this help and exit\n"
        "       --version               show version and exit\n");
}

bool onyx_args_parse(int argc, char ** argv, onyx_args & out) {
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
        if (is("--context-shift"))    { out.ctx_shift = true;  continue; }
        if (is("--no-context-shift")) { out.ctx_shift = false; continue; }

        const char * val = nullptr;

        if (is("-m", "--model"))              { if (!(val = need_value(i, argv[i]))) return false; out.model_path = val; continue; }
        if (is("-a", "--alias"))              { if (!(val = need_value(i, argv[i]))) return false; out.alias = val; continue; }
        if (is("--chat-template"))            { if (!(val = need_value(i, argv[i]))) return false; out.chat_template = val; continue; }
        if (is("--chat-template-file"))       { if (!(val = need_value(i, argv[i]))) return false; out.chat_template_file = val; continue; }
        if (is("--host"))                     { if (!(val = need_value(i, argv[i]))) return false; out.host = val; continue; }
        if (is("--api-key"))                  { if (!(val = need_value(i, argv[i]))) return false; out.api_key = val; continue; }
        if (is("--pooling"))                  { if (!(val = need_value(i, argv[i]))) return false; out.pooling = val; continue; }
        if (is("--mmproj"))                   { if (!(val = need_value(i, argv[i]))) return false; out.mmproj_path = val; continue; }
        if (is("--convert-dir"))              { if (!(val = need_value(i, argv[i]))) return false; out.convert_dir = val; continue; }
        if (is("--reasoning-budget-message")) { if (!(val = need_value(i, argv[i]))) return false; out.reasoning_budget_message = val; continue; }

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
        if (is("--keep"))                     { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_keep = n; continue; }
        if (is("--reasoning-budget"))         { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.reasoning_budget = n; continue; }
        if (is("-s", "--seed"))               { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.seed = (uint32_t) n; continue; }

        fprintf(stderr, "error: unknown argument '%s' (see --help)\n", arg.c_str());
        return false;
    }

    if (out.model_path.empty()) {
        fprintf(stderr, "error: --model is required (see --help)\n");
        return false;
    }

    if (out.alias.empty()) {
        // default alias: model file/dir name without directory or extension
        std::string base = out.model_path;
        while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
            base.pop_back();
        }
        const size_t slash = base.find_last_of("/\\");
        if (slash != std::string::npos) {
            base = base.substr(slash + 1);
        }
        for (const char * ext : {".gguf", ".safetensors"}) {
            const size_t dot = base.rfind(ext);
            if (dot != std::string::npos && dot + strlen(ext) == base.size()) {
                base = base.substr(0, dot);
                break;
            }
        }
        out.alias = base.empty() ? "model" : base;
    }

    return true;
}
