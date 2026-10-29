// Command-line parsing: the parser is half of the --help contract.
#include "check.h"

#include "args.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

// Runs the parser on a command line; stderr from expected refusals is fine.
bool parse(std::initializer_list<const char *> argv_tail, onyx_args & out) {
    std::vector<char *> argv{const_cast<char *>("onyx-engine")};
    for (const char * a : argv_tail) {
        argv.push_back(const_cast<char *>(a));
    }
    out = onyx_args{};
    return onyx_args_parse((int) argv.size(), argv.data(), out);
}

bool parses(std::initializer_list<const char *> argv_tail) {
    onyx_args a;
    return parse(argv_tail, a);
}

} // namespace

TEST(args, defaults) {
    onyx_args a;
    CHECK(parse({"-m", "models/x.gguf"}, a));
    CHECK_EQ(a.host, std::string("127.0.0.1"));
    CHECK_EQ(a.port, 8080);
    CHECK_EQ(a.n_ctx, 4096);
    CHECK_EQ(a.n_parallel, 1);
    CHECK_EQ(a.max_queue, 0);
    CHECK_EQ(a.tick_tokens, 0);
    CHECK(!a.metrics);
    CHECK_EQ(a.flash_attn, -1);
    CHECK_EQ(a.queue_timeout, 0);
    CHECK_EQ(a.cache_type_k, std::string("f16"));
    CHECK_EQ(a.cache_type_v, std::string("f16"));
    CHECK(!a.cpu_moe);
    CHECK_EQ(a.n_cpu_moe, 0);
    CHECK(a.tensor_overrides.empty());
}

TEST(args, model_is_required) {
    CHECK(!parses({}));
    CHECK(!parses({"--port", "9000"}));
}

TEST(args, alias_defaults_to_file_stem) {
    onyx_args a;
    CHECK(parse({"-m", "/models/qwen3-8b.Q4_K_M.gguf"}, a));
    CHECK_EQ(a.alias, std::string("qwen3-8b.Q4_K_M"));
    CHECK(parse({"-m", "C:\\models\\Llama-3-8B-hf\\"}, a));
    CHECK_EQ(a.alias, std::string("Llama-3-8B-hf"));
    CHECK(parse({"-m", "dir/model.safetensors"}, a));
    CHECK_EQ(a.alias, std::string("model"));
    CHECK(parse({"-m", "x.gguf", "--alias", "chosen"}, a));
    CHECK_EQ(a.alias, std::string("chosen"));
}

TEST(args, help_and_version_short_circuit) {
    onyx_args a;
    CHECK(parse({"--help"}, a));
    CHECK(a.show_help);
    CHECK(parse({"--version"}, a));
    CHECK(a.show_version);
}

TEST(args, unknown_flags_are_refused) {
    CHECK(!parses({"-m", "x.gguf", "--no-such-flag"}));
    CHECK(!parses({"-m", "x.gguf", "--fit", "on"}));              // not implemented, so not accepted
}

TEST(args, integers_are_strict) {
    CHECK(!parses({"-m", "x.gguf", "--port", "80x"}));
    CHECK(!parses({"-m", "x.gguf", "-c", ""}));
    CHECK(!parses({"-m", "x.gguf", "-c", "99999999999"}));
    CHECK(!parses({"-m", "x.gguf", "-c"}));                       // missing value
}

TEST(args, port_range_allows_zero) {
    onyx_args a;
    CHECK(parse({"-m", "x.gguf", "--port", "0"}, a));
    CHECK_EQ(a.port, 0);
    CHECK(parses({"-m", "x.gguf", "--port", "65535"}));
    CHECK(!parses({"-m", "x.gguf", "--port", "-1"}));
    CHECK(!parses({"-m", "x.gguf", "--port", "65536"}));
}

TEST(args, flash_attn_needs_a_value) {
    onyx_args a;
    CHECK(parse({"-m", "x.gguf", "-fa", "on"}, a));
    CHECK_EQ(a.flash_attn, 1);
    CHECK(parse({"-m", "x.gguf", "--flash-attn", "0"}, a));
    CHECK_EQ(a.flash_attn, 0);
    CHECK(parse({"-m", "x.gguf", "-fa", "auto"}, a));
    CHECK_EQ(a.flash_attn, -1);
    CHECK(!parses({"-m", "x.gguf", "-fa", "yes"}));
    CHECK(!parses({"-m", "x.gguf", "-fa"}));
}

TEST(args, operator_flags) {
    onyx_args a;
    CHECK(parse({"-m", "x.gguf", "--max-queue", "8", "--tick-tokens", "256", "--metrics"}, a));
    CHECK_EQ(a.max_queue, 8);
    CHECK_EQ(a.tick_tokens, 256);
    CHECK(a.metrics);
    CHECK(!parses({"-m", "x.gguf", "--max-queue", "-1"}));
    CHECK(!parses({"-m", "x.gguf", "--tick-tokens", "-5"}));
}

TEST(args, non_loopback_host_needs_a_key) {
    CHECK(!parses({"-m", "x.gguf", "--host", "0.0.0.0"}));
    CHECK(!parses({"-m", "x.gguf", "--host", "192.168.1.20"}));
    CHECK(parses({"-m", "x.gguf", "--host", "0.0.0.0", "--api-key", "k"}));
    CHECK(parses({"-m", "x.gguf", "--host", "0.0.0.0", "--allow-unauthenticated"}));
    CHECK(parses({"-m", "x.gguf", "--host", "127.0.0.2"}));
    CHECK(parses({"-m", "x.gguf", "--host", "localhost"}));
    CHECK(parses({"-m", "x.gguf", "--host", "::1"}));
}

TEST(args, loopback_hosts) {
    CHECK(onyx_host_is_loopback("127.0.0.1"));
    CHECK(onyx_host_is_loopback("127.1.2.3"));
    CHECK(onyx_host_is_loopback("localhost"));
    CHECK(onyx_host_is_loopback("::1"));
    CHECK(onyx_host_is_loopback("[::1]"));
    CHECK(!onyx_host_is_loopback("0.0.0.0"));
    CHECK(!onyx_host_is_loopback("::"));
    CHECK(!onyx_host_is_loopback("10.0.0.5"));
    CHECK(!onyx_host_is_loopback("localhost.example.com"));
    CHECK(!onyx_host_is_loopback("127.0.0.1.example.com"));
}

TEST(args, pooling_rules) {
    CHECK(parses({"-m", "x.gguf", "--embedding", "--pooling", "mean"}));
    CHECK(!parses({"-m", "x.gguf", "--pooling", "mean"}));             // needs --embedding
    CHECK(!parses({"-m", "x.gguf", "--embedding", "--pooling", "max"}));
}

TEST(args, other_ranges) {
    CHECK(!parses({"-m", "x.gguf", "-np", "0"}));
    CHECK(!parses({"-m", "x.gguf", "-b", "0"}));
    CHECK(!parses({"-m", "x.gguf", "-ub", "0"}));
    CHECK(!parses({"-m", "x.gguf", "-c", "-1"}));
    CHECK(!parses({"-m", "x.gguf", "--cache-reuse", "-1"}));
    CHECK(parses({"-m", "x.gguf", "-c", "0"}));                       // the model's own context
}

TEST(args, queue_timeout) {
    onyx_args a;
    CHECK(parse({"-m", "x.gguf", "--queue-timeout", "30"}, a));
    CHECK_EQ(a.queue_timeout, 30);
    CHECK(!parses({"-m", "x.gguf", "--queue-timeout", "-1"}));
    CHECK(!parses({"-m", "x.gguf", "--queue-timeout"}));
}

TEST(args, cache_types) {
    onyx_args a;
    CHECK(parse({"-m", "x.gguf", "-ctk", "q8_0", "--cache-type-v", "q8_0"}, a));
    CHECK_EQ(a.cache_type_k, std::string("q8_0"));
    CHECK_EQ(a.cache_type_v, std::string("q8_0"));
    CHECK(parse({"-m", "x.gguf", "--cache-type-k", "bf16", "-ctv", "iq4_nl"}, a));
    CHECK_EQ(a.cache_type_k, std::string("bf16"));
    CHECK_EQ(a.cache_type_v, std::string("iq4_nl"));
    CHECK(!parses({"-m", "x.gguf", "-ctk", "q3_k"}));
    CHECK(!parses({"-m", "x.gguf", "-ctk", "Q8_0"}));                 // llama-server's spelling only
    CHECK(!parses({"-m", "x.gguf", "-ctv"}));
    // a quantized V cache needs flash attention; K alone does not
    CHECK(!parses({"-m", "x.gguf", "-ctv", "q8_0", "-fa", "off"}));
    CHECK(parses({"-m", "x.gguf", "-ctv", "q8_0", "-fa", "on"}));
    CHECK(parses({"-m", "x.gguf", "-ctk", "q8_0", "-fa", "off"}));
    CHECK(parses({"-m", "x.gguf", "-ctv", "bf16", "-fa", "off"}));
    CHECK(onyx_cache_type_is_quantized("q4_0"));
    CHECK(!onyx_cache_type_is_quantized("f32"));
    CHECK(!onyx_cache_type_is_quantized("nonsense"));
}

TEST(args, moe_offload) {
    onyx_args a;
    CHECK(parse({"-m", "x.gguf", "--cpu-moe"}, a));
    CHECK(a.cpu_moe);
    CHECK(parse({"-m", "x.gguf", "-cmoe"}, a));
    CHECK(a.cpu_moe);
    CHECK(parse({"-m", "x.gguf", "--n-cpu-moe", "12"}, a));
    CHECK_EQ(a.n_cpu_moe, 12);
    CHECK(parse({"-m", "x.gguf", "-ncmoe", "3"}, a));
    CHECK_EQ(a.n_cpu_moe, 3);
    CHECK(!parses({"-m", "x.gguf", "--n-cpu-moe", "-2"}));
}

TEST(args, override_tensor) {
    onyx_args a;
    CHECK(parse({"-m", "x.gguf", "-ot", "exps=CPU", "--override-tensor", "blk\\.0\\.attn=CUDA0,ffn=CPU"}, a));
    CHECK_EQ(a.tensor_overrides.size(), (size_t) 3);
    CHECK_EQ(a.tensor_overrides[0].first, std::string("exps"));
    CHECK_EQ(a.tensor_overrides[0].second, std::string("CPU"));
    CHECK_EQ(a.tensor_overrides[1].first, std::string("blk\\.0\\.attn"));
    CHECK_EQ(a.tensor_overrides[1].second, std::string("CUDA0"));
    CHECK_EQ(a.tensor_overrides[2].first, std::string("ffn"));
    CHECK(!parses({"-m", "x.gguf", "-ot", "exps"}));
    CHECK(!parses({"-m", "x.gguf", "-ot", "=CPU"}));
    CHECK(!parses({"-m", "x.gguf", "-ot", "exps="}));
    CHECK(!parses({"-m", "x.gguf", "-ot", "a=CPU,"}));
    CHECK(!parses({"-m", "x.gguf", "-ot"}));

    std::vector<std::pair<std::string, std::string>> out;
    CHECK(onyx_parse_tensor_overrides("x{1,2}=CPU", out) == false);   // a comma always separates items
    out.clear();
    CHECK(onyx_parse_tensor_overrides("a=b=CPU", out));               // the last '=' splits
    CHECK_EQ(out[0].first, std::string("a=b"));
    CHECK_EQ(out[0].second, std::string("CPU"));
}
