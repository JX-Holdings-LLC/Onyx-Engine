// The JSON type onyx-engine's own code uses: nlohmann::ordered_json from
// third_party/nlohmann (the version llama.cpp's pin vendors), not llama.cpp's
// common_json wrapper. Ordered, so object keys keep the order they were set
// in, as common_json's did.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

using onyx_json       = nlohmann::ordered_json;
using onyx_json_error = nlohmann::ordered_json::exception;  // every error the library throws

// dump(), with invalid UTF-8 replaced (U+FFFD) instead of throwing: for
// generated text, which can end inside a multi-byte character.
inline std::string onyx_json_dump_safe(const onyx_json & j) {
    return j.dump(-1, ' ', false, onyx_json::error_handler_t::replace);
}
