#include "request.h"

#include <cmath>

std::vector<std::string> onyx_parse_stop(const onyx_json & body) {
    std::vector<std::string> stop;
    if (!body.contains("stop")) {
        return stop;
    }
    const onyx_json & s = body.at("stop");
    if (s.is_string()) {
        std::string v = s.get<std::string>();
        if (!v.empty()) {
            stop.push_back(std::move(v));
        }
    } else if (s.is_array()) {
        for (size_t i = 0; i < s.size(); i++) {
            std::string v = s[i].get<std::string>();
            if (!v.empty()) {
                stop.push_back(std::move(v));
            }
        }
    }
    return stop;
}

int32_t onyx_parse_max_tokens(const onyx_json & body) {
    for (const char * key : {"max_completion_tokens", "max_tokens", "n_predict"}) {
        if (body.contains(key) && !body.at(key).is_null()) {
            return body.at(key).get<int32_t>();
        }
    }
    return -1;
}

onyx_sampling_params onyx_parse_sampling(const onyx_json & body, uint32_t default_seed) {
    onyx_sampling_params s;
    s.seed = default_seed;

    if (body.contains("temperature") && !body.at("temperature").is_null()) s.temp = body.at("temperature").get<float>();
    if (body.contains("top_p"))             s.top_p           = body.at("top_p").get<float>();
    if (body.contains("top_k"))             s.top_k           = body.at("top_k").get<int32_t>();
    if (body.contains("min_p"))             s.min_p           = body.at("min_p").get<float>();
    if (body.contains("seed") && !body.at("seed").is_null()) s.seed = (uint32_t) body.at("seed").get<int64_t>();
    if (body.contains("repeat_penalty"))    s.penalty_repeat  = body.at("repeat_penalty").get<float>();
    if (body.contains("repeat_last_n"))     s.penalty_last_n  = body.at("repeat_last_n").get<int32_t>();
    if (body.contains("presence_penalty") && !body.at("presence_penalty").is_null())
        s.penalty_present = body.at("presence_penalty").get<float>();
    if (body.contains("frequency_penalty") && !body.at("frequency_penalty").is_null())
        s.penalty_freq = body.at("frequency_penalty").get<float>();

    return s;
}

void onyx_truncate_embedding(std::vector<float> & v, size_t dims) {
    if (v.size() > dims) {
        v.resize(dims);
    }
    double sum = 0.0;
    for (const float x : v) {
        sum += (double) x * x;
    }
    if (sum > 0.0) {
        const float norm = (float) std::sqrt(sum);
        for (float & x : v) {
            x /= norm;
        }
    }
}
