#include "args.h"
#include "http_util.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <limits>
#include <string>
#include <vector>

static bool parse_int64(const char * s, int64_t & out) {
    char * end = nullptr;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) {
        fprintf(stderr, "error: invalid integer value '%s'\n", s);
        return false;
    }
    out = (int64_t) v;
    return true;
}

static bool parse_int(const char * s, int32_t & out) {
    char * end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE ||
        v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
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
        "Onyx Engine - a small, strict, supervisable OpenAI-compatible model server.\n"
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
        "       --chat-template SOURCE  override with Jinja chat template source\n"
        "       --chat-template-file F  override the model's chat template from a file\n"
        "       --jinja                 apply the model's jinja chat template (always on)\n"
        "       --no-webui              accepted for compatibility (onyx-engine has no web UI)\n"
        "\n"
        "network:\n"
        "       --host HOST             address to bind (default: 127.0.0.1); any other\n"
        "                               than loopback requires --api-key\n"
        "       --port PORT             port to listen on, 0 = any free port, reported\n"
        "                               on stderr and in /props (default: 8080)\n"
        "       --api-key KEY           require this bearer token except on /health\n"
        "       --api-key-file PATH     read the --api-key token from a file (a trailing\n"
        "                               newline is dropped); without either flag, the\n"
        "                               " ONYX_API_KEY_ENV " environment variable is used\n"
        "       --allow-unauthenticated\n"
        "                               allow a non-loopback --host without --api-key\n"
        "       --metrics               serve Prometheus metrics on /metrics\n"
        "       --timeout N             seconds a response may wait to be written, and the\n"
        "                               most a request may take to arrive (that one is\n"
        "                               capped at 30) (default: 600)\n"
        "       --max-request-bytes N   largest request body accepted; larger ones get\n"
        "                               413 (default: 104857600, 100 MiB)\n"
        "\n"
        "compute:\n"
        "  -c,  --ctx-size N            context size in tokens, 0 = model default (default: 4096)\n"
        "  -b,  --batch-size N          logical batch size (default: 2048)\n"
        "  -ub, --ubatch-size N         physical batch size (default: 512)\n"
        "  -ngl, --n-gpu-layers N       layers to offload to GPU, -1 = all (default: -1)\n"
        "       --gpu-layers N         alias of --n-gpu-layers\n"
        "  -t,  --threads N             generation threads, -1 = auto (default: -1)\n"
        "  -tb, --threads-batch N       prompt processing threads, -1 = same as --threads\n"
        "  -np, --parallel N            concurrent request slots; requests beyond this\n"
        "                               queue (default: 1)\n"
        "       --max-queue N           requests allowed to wait for a slot; beyond it,\n"
        "                               answer 503 with Retry-After, 0 = unbounded\n"
        "                               (default: 0)\n"
        "       --tick-tokens N         tokens per batching tick, decode rows first,\n"
        "                               0 = --batch-size (default: 0)\n"
        "       --queue-timeout N       seconds a request may wait for a slot; beyond\n"
        "                               it, answer 503 with Retry-After, 0 = no limit\n"
        "                               (default: 0)\n"
        // "on|off|auto" (pipes), not "on, off, auto": supervisors probe this
        // line to choose between `-fa <value>` and a bare `-fa` (JX Runtime's
        // llama.cpp adapter matches /on\|off\|auto|'on'/ against it), and the
        // comma form reads as the old bare-toggle flag — which onyx-engine
        // rejects ("error: -fa requires a value"). The pipe form is also what
        // the --flash-attn error message and llama-server's own help use.
        "  -fa, --flash-attn VAL        flash attention: on|off|auto|1|0 (default: auto)\n"
        "       --mlock                 lock model memory in RAM\n"
        "       --no-mmap               do not memory-map the model file\n"
        "  -s,  --seed N                default RNG seed (default: random)\n"
        "\n"
        "memory:\n"
        "  -ctk, --cache-type-k TYPE    KV cache type for K: f32, f16, bf16, q8_0, q4_0,\n"
        "                               q4_1, iq4_nl, q5_0, q5_1 (default: f16)\n"
        "  -ctv, --cache-type-v TYPE    KV cache type for V, same values (default: f16);\n"
        "                               a quantized V cache needs flash attention\n"
        "  -cmoe, --cpu-moe             keep all MoE expert weights in host memory (CPU)\n"
        "  -ncmoe, --n-cpu-moe N        keep the MoE expert weights of the first N layers\n"
        "                               in host memory (CPU) (default: 0)\n"
        "  -ot, --override-tensor P=B   place tensors whose name matches regex P in buffer\n"
        "                               type B (e.g. CPU); comma-separated, repeatable\n"
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

static const char * const ONYX_CACHE_TYPES[] = {
    "f32", "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl", "q5_0", "q5_1",
};

bool onyx_cache_type_is_valid(const std::string & name) {
    for (const char * t : ONYX_CACHE_TYPES) {
        if (name == t) {
            return true;
        }
    }
    return false;
}

bool onyx_cache_type_is_quantized(const std::string & name) {
    return onyx_cache_type_is_valid(name) && name != "f32" && name != "f16" && name != "bf16";
}

bool onyx_parse_tensor_overrides(const std::string & value,
                                 std::vector<std::pair<std::string, std::string>> & out) {
    // llama-server's form: PATTERN=BUFT, comma-separated. A pattern can hold
    // '=' itself only before the last one, so split each item on its last '='.
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) {
            comma = value.size();
        }
        const std::string item = value.substr(start, comma - start);
        const size_t eq = item.rfind('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == item.size()) {
            fprintf(stderr, "error: invalid --override-tensor value '%s' (expected PATTERN=BUFFER_TYPE)\n",
                    item.c_str());
            return false;
        }
        out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
        start = comma + 1;
    }
    return true;
}

bool onyx_read_api_key_file(const std::string & path, std::string & key) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "error: cannot read --api-key-file '%s'\n", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    key = ss.str();
    if (!key.empty() && key.back() == '\n') {
        key.pop_back();
        if (!key.empty() && key.back() == '\r') {
            key.pop_back();
        }
    }
    if (key.empty()) {
        fprintf(stderr, "error: --api-key-file '%s' holds no key\n", path.c_str());
        return false;
    }
    return true;
}

bool onyx_host_is_loopback(const std::string & host) {
    return host == "localhost" || host == "::1" || host == "[::1]" || onyx_is_ipv4_loopback(host);
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
        if (is("--no-webui"))      { out.no_webui = true; continue; }
        if (is("--metrics"))       { out.metrics = true; continue; }
        if (is("--allow-unauthenticated")) { out.allow_unauthenticated = true; continue; }
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
        if (is("--api-key-file"))             { if (!(val = need_value(i, argv[i]))) return false; out.api_key_file = val; continue; }
        if (is("--pooling"))                  { if (!(val = need_value(i, argv[i]))) return false; out.pooling = val; continue; }
        if (is("--mmproj"))                   { if (!(val = need_value(i, argv[i]))) return false; out.mmproj_path = val; continue; }
        if (is("--convert-dir"))              { if (!(val = need_value(i, argv[i]))) return false; out.convert_dir = val; continue; }
        if (is("--reasoning-budget-message")) { if (!(val = need_value(i, argv[i]))) return false; out.reasoning_budget_message = val; continue; }
        if (is("-cmoe", "--cpu-moe"))         { out.cpu_moe = true; continue; }

        if (is("-ctk", "--cache-type-k", "-ctv") || is("--cache-type-v")) {
            if (!(val = need_value(i, argv[i]))) return false;
            if (!onyx_cache_type_is_valid(val)) {
                fprintf(stderr, "error: invalid %s value '%s' (expected f32, f16, bf16, q8_0, q4_0, q4_1, "
                                "iq4_nl, q5_0 or q5_1)\n", arg.c_str(), val);
                return false;
            }
            (arg == "-ctk" || arg == "--cache-type-k" ? out.cache_type_k : out.cache_type_v) = val;
            continue;
        }
        if (is("-ot", "--override-tensor")) {
            if (!(val = need_value(i, argv[i])) || !onyx_parse_tensor_overrides(val, out.tensor_overrides)) return false;
            continue;
        }

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
        if (is("--max-queue"))                { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.max_queue = n; continue; }
        if (is("--tick-tokens"))              { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.tick_tokens = n; continue; }
        if (is("--cache-reuse"))              { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.cache_reuse = n; continue; }
        if (is("--keep"))                     { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_keep = n; continue; }
        if (is("--reasoning-budget"))         { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.reasoning_budget = n; continue; }
        if (is("--queue-timeout"))            { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.queue_timeout = n; continue; }
        if (is("-ncmoe", "--n-cpu-moe"))      { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.n_cpu_moe = n; continue; }
        if (is("--timeout"))                  { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.timeout = n; continue; }
        if (is("--max-request-bytes")) {
            int64_t v = 0;
            if (!(val = need_value(i, argv[i])) || !parse_int64(val, v)) return false;
            out.max_request_bytes = v;
            continue;
        }
        if (is("-s", "--seed"))               { if (!(val = need_value(i, argv[i])) || !parse_int(val, n)) return false; out.seed = (uint32_t) n; continue; }

        fprintf(stderr, "error: unknown argument '%s' (see --help)\n", arg.c_str());
        return false;
    }

    if (out.model_path.empty()) {
        fprintf(stderr, "error: --model is required (see --help)\n");
        return false;
    }
    if (out.port < 0 || out.port > 65535 || out.n_parallel < 1 || out.cache_reuse < 0 ||
        out.n_batch < 1 || out.n_ubatch < 1 || out.n_ctx < 0 || out.max_queue < 0 || out.tick_tokens < 0 ||
        out.queue_timeout < 0 || out.n_cpu_moe < 0) {
        fprintf(stderr, "error: invalid port, parallel, max-queue, queue-timeout, tick-tokens, n-cpu-moe, "
                        "cache-reuse, batch or context value\n");
        return false;
    }
    if (out.timeout < 1) {
        fprintf(stderr, "error: --timeout must be at least 1 second\n");
        return false;
    }
    if (out.max_request_bytes < 1) {
        fprintf(stderr, "error: --max-request-bytes must be at least 1\n");
        return false;
    }
    // the key: --api-key, else --api-key-file, else the environment; the
    // same constant-time compare and --host rule apply to all three
    if (!out.api_key_file.empty()) {
        if (!out.api_key.empty()) {
            fprintf(stderr, "error: --api-key and --api-key-file are exclusive\n");
            return false;
        }
        if (!onyx_read_api_key_file(out.api_key_file, out.api_key)) {
            return false;
        }
    } else if (out.api_key.empty()) {
        const char * env = getenv(ONYX_API_KEY_ENV);
        if (env && *env) {
            out.api_key = env;
        }
    }
    if (onyx_cache_type_is_quantized(out.cache_type_v) && out.flash_attn == 0) {
        fprintf(stderr, "error: --cache-type-v %s needs flash attention; drop -fa off\n", out.cache_type_v.c_str());
        return false;
    }
    if (!onyx_host_is_loopback(out.host) && out.api_key.empty() && !out.allow_unauthenticated) {
        fprintf(stderr, "error: --host %s accepts connections from other machines; set --api-key, "
                        "or pass --allow-unauthenticated to serve without one\n", out.host.c_str());
        return false;
    }
    if (out.pooling != "" && out.pooling != "none" && out.pooling != "mean" &&
        out.pooling != "cls" && out.pooling != "last" && out.pooling != "rank") {
        fprintf(stderr, "error: invalid --pooling value '%s'\n", out.pooling.c_str());
        return false;
    }
    if (!out.embedding && !out.pooling.empty()) {
        fprintf(stderr, "error: --pooling requires --embedding\n");
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
