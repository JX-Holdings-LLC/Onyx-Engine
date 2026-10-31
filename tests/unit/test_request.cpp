// Request-body parsing (src/request.cpp) and the sampler's trigger patterns.
#include "check.h"

#include "request.h"
#include "sampler.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

onyx_json body(const char * text) {
    return onyx_json::parse(text);
}

} // namespace

TEST(request, stop_string_array_empty_and_absent) {
    CHECK(onyx_parse_stop(body("{}")).empty());
    CHECK(onyx_parse_stop(body(R"({"stop": null})")).empty());

    const auto one = onyx_parse_stop(body(R"({"stop": "END"})"));
    CHECK_EQ(one.size(), (size_t) 1);
    CHECK_EQ(one[0], std::string("END"));

    CHECK(onyx_parse_stop(body(R"({"stop": ""})")).empty());

    const auto many = onyx_parse_stop(body(R"({"stop": ["a", "", "b"]})"));
    CHECK_EQ(many.size(), (size_t) 2);
    CHECK_EQ(many[0], std::string("a"));
    CHECK_EQ(many[1], std::string("b"));

    CHECK_THROWS(onyx_parse_stop(body(R"({"stop": [1]})")));
}

TEST(request, max_tokens_precedence) {
    CHECK_EQ(onyx_parse_max_tokens(body("{}")), -1);
    CHECK_EQ(onyx_parse_max_tokens(body(R"({"n_predict": 5})")), 5);
    CHECK_EQ(onyx_parse_max_tokens(body(R"({"max_tokens": 7, "n_predict": 5})")), 7);
    CHECK_EQ(onyx_parse_max_tokens(body(R"({"max_completion_tokens": 9, "max_tokens": 7, "n_predict": 5})")), 9);
    // null is skipped, not read as 0
    CHECK_EQ(onyx_parse_max_tokens(body(R"({"max_completion_tokens": null, "max_tokens": 7})")), 7);
    CHECK_EQ(onyx_parse_max_tokens(body(R"({"max_tokens": null})")), -1);
}

TEST(request, sampling_defaults_and_overrides) {
    const onyx_sampling_params d = onyx_parse_sampling(body("{}"), 1234);
    CHECK_EQ(d.seed, (uint32_t) 1234);
    CHECK_EQ(d.temp, 0.80f);
    CHECK_EQ(d.top_k, 40);
    CHECK_EQ(d.top_p, 0.95f);
    CHECK_EQ(d.min_p, 0.05f);
    CHECK_EQ(d.penalty_last_n, 64);
    CHECK_EQ(d.penalty_repeat, 1.0f);
    CHECK_EQ(d.penalty_freq, 0.0f);
    CHECK_EQ(d.penalty_present, 0.0f);
    CHECK(d.grammar.empty());

    const onyx_sampling_params s = onyx_parse_sampling(body(R"({
        "temperature": 0, "top_k": 1, "top_p": 0.5, "min_p": 0.2, "seed": 7,
        "repeat_penalty": 1.3, "repeat_last_n": 16, "presence_penalty": 0.5, "frequency_penalty": 0.25})"), 1234);
    CHECK_EQ(s.seed, (uint32_t) 7);
    CHECK_EQ(s.temp, 0.0f);
    CHECK_EQ(s.top_k, 1);
    CHECK_EQ(s.top_p, 0.5f);
    CHECK_EQ(s.min_p, 0.2f);
    CHECK_EQ(s.penalty_repeat, 1.3f);
    CHECK_EQ(s.penalty_last_n, 16);
    CHECK_EQ(s.penalty_present, 0.5f);
    CHECK_EQ(s.penalty_freq, 0.25f);

    // null leaves the default (and the --seed) in place
    const onyx_sampling_params n = onyx_parse_sampling(body(R"({"temperature": null, "seed": null})"), 99);
    CHECK_EQ(n.temp, 0.80f);
    CHECK_EQ(n.seed, (uint32_t) 99);

    CHECK_THROWS(onyx_parse_sampling(body(R"({"top_k": "many"})"), 0));
}

TEST(request, truncate_embedding_renormalises) {
    std::vector<float> v = {3.0f, 4.0f, 12.0f};
    onyx_truncate_embedding(v, 2);
    CHECK_EQ(v.size(), (size_t) 2);
    CHECK(std::fabs(v[0] - 0.6f) < 1e-6f);
    CHECK(std::fabs(v[1] - 0.8f) < 1e-6f);

    std::vector<float> zero = {0.0f, 0.0f};
    onyx_truncate_embedding(zero, 5);
    CHECK_EQ(zero.size(), (size_t) 2);
    CHECK_EQ(zero[0], 0.0f);
}

TEST(request, trigger_patterns) {
    onyx_grammar_trigger word;
    word.kind  = ONYX_TRIGGER_WORD;
    word.value = "<tool_call>(x).*";
    CHECK_EQ(onyx_trigger_pattern(word), std::string("<tool_call>\\(x\\)\\.\\*"));

    onyx_grammar_trigger full;
    full.kind  = ONYX_TRIGGER_PATTERN_FULL;
    full.value = "\\s*\\{";
    CHECK_EQ(onyx_trigger_pattern(full), std::string("^\\s*\\{$"));
    full.value = "^a$";
    CHECK_EQ(onyx_trigger_pattern(full), std::string("^a$"));
    full.value = "";
    CHECK_EQ(onyx_trigger_pattern(full), std::string("^$"));

    onyx_grammar_trigger pat;
    pat.kind  = ONYX_TRIGGER_PATTERN;
    pat.value = "[a-z]+";
    CHECK_EQ(onyx_trigger_pattern(pat), std::string("[a-z]+"));

    onyx_grammar_trigger tok;
    tok.kind  = ONYX_TRIGGER_TOKEN;
    tok.token = 42;
    CHECK(onyx_trigger_pattern(tok).empty());
}
