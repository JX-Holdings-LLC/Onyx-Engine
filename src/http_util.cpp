// Request-level helpers with no dependency on the engine or llama.cpp, kept
// apart from server.cpp so the unit tests (tests/unit) can exercise them.
#include "http_util.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <stdexcept>

// X-Request-Id is echoed only when it is short, printable ASCII: it is a
// caller's correlation id, never something to reflect unchecked into a header.
bool onyx_is_ipv4_loopback(const std::string & s) {
    // exactly four decimal octets, 0-255, the first 127: a DNS name such as
    // "127.0.0.1.attacker.example" must not pass for an address
    int    octets = 0;
    size_t i      = 0;
    while (i <= s.size()) {
        size_t j = i;
        int    v = 0;
        while (j < s.size() && std::isdigit((unsigned char) s[j]) && j - i < 3) {
            v = v * 10 + (s[j] - '0');
            j++;
        }
        if (j == i || v > 255 || (octets == 0 && v != 127)) {
            return false;
        }
        octets++;
        if (j == s.size()) {
            return octets == 4;
        }
        if (s[j] != '.' || octets == 4) {
            return false;
        }
        i = j + 1;
    }
    return false;
}

bool valid_request_id(const std::string & id) {
    if (id.empty() || id.size() > 128) {
        return false;
    }
    for (unsigned char c : id) {
        if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

// Compares in time that depends only on the lengths, so a caller cannot find
// the API key one byte at a time by timing 401s.
bool constant_time_equals(const std::string & a, const std::string & b) {
    const size_t n = std::max(a.size(), b.size());
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    for (size_t i = 0; i < n; i++) {
        const unsigned char x = i < a.size() ? (unsigned char) a[i] : 0;
        const unsigned char y = i < b.size() ? (unsigned char) b[i] : 0;
        diff |= (unsigned char) (x ^ y);
    }
    return diff == 0;
}

// The host part of a Host header: "[::1]:8080" -> "[::1]", "a:80" -> "a".
std::string host_header_name(const std::string & host) {
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        return close == std::string::npos ? host : host.substr(0, close + 1);
    }
    const size_t colon = host.find(':');
    return colon == std::string::npos ? host : host.substr(0, colon);
}

// DNS-rebinding guard for a loopback-bound server: a browser tricked into
// talking to 127.0.0.1 still sends the attacker's name in Host.
bool host_header_is_loopback(const std::string & host) {
    std::string name = host_header_name(host);
    for (auto & c : name) {
        c = (char) std::tolower((unsigned char) c);
    }
    if (name == "localhost" || name == "[::1]" || onyx_is_ipv4_loopback(name)) {
        return true;
    }
    const std::string suffix = ".localhost";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Minimal base64 decoder (same shape as llama-server's). Stops at the first
// character that is neither base64 nor padding.
onyx_media_buffer base64_decode(const std::string & encoded) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    onyx_media_buffer out;
    uint8_t quad[4];
    int n = 0;

    for (char c : encoded) {
        const size_t idx = chars.find(c);
        if (idx == std::string::npos) {
            break;   // '=' padding, whitespace or garbage: stop here
        }
        quad[n++] = (uint8_t) idx;
        if (n == 4) {
            out.push_back((unsigned char) ((quad[0] << 2) | (quad[1] >> 4)));
            out.push_back((unsigned char) (((quad[1] & 0x0f) << 4) | (quad[2] >> 2)));
            out.push_back((unsigned char) (((quad[2] & 0x03) << 6) | quad[3]));
            n = 0;
        }
    }
    for (int i = n; i < 4; i++) {
        quad[i] = 0;
    }
    if (n > 1) {
        out.push_back((unsigned char) ((quad[0] << 2) | (quad[1] >> 4)));
    }
    if (n > 2) {
        out.push_back((unsigned char) (((quad[1] & 0x0f) << 4) | (quad[2] >> 2)));
    }
    return out;
}

// Standard base64 with padding, for /v1/embeddings' encoding_format "base64".
std::string base64_encode(const unsigned char * data, size_t n) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t b = (uint32_t) data[i] << 16 |
                           (i + 1 < n ? (uint32_t) data[i + 1] << 8 : 0) |
                           (i + 2 < n ? (uint32_t) data[i + 2] : 0);
        out.push_back(chars[(b >> 18) & 63]);
        out.push_back(chars[(b >> 12) & 63]);
        out.push_back(i + 1 < n ? chars[(b >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? chars[b & 63] : '=');
    }
    return out;
}

// OpenAI's base64 embedding: the float32 vector's bytes, little-endian.
std::string embedding_base64(const std::vector<float> & v) {
    std::vector<unsigned char> bytes(v.size() * 4);
    for (size_t i = 0; i < v.size(); i++) {
        uint32_t u;
        memcpy(&u, &v[i], 4);
        for (int k = 0; k < 4; k++) {
            bytes[i * 4 + (size_t) k] = (unsigned char) (u >> (8 * k));
        }
    }
    return base64_encode(bytes.data(), bytes.size());
}

// onyx-engine performs no network or filesystem I/O on behalf of a request, so a
// media payload can never be a location the server would have to go fetch.
// Checked before any capability check: this holds whatever projector is loaded.
void reject_media_url(const std::string & url, const char * what) {
    for (const char * scheme : {"http://", "https://", "file://"}) {
        if (url.rfind(scheme, 0) == 0) {
            throw std::runtime_error(std::string(scheme) + " " + what +
                                     " URLs are not supported: onyx-engine does not fetch remote or local "
                                     "resources. Inline the data as a 'data:<mime>;base64,...' URI or a "
                                     "bare base64 string.");
        }
    }
}

// Resolves one media payload to raw file bytes: a data: URI or bare base64.
onyx_media_buffer decode_media_payload(const std::string & url, const char * what) {
    std::string payload = url;
    if (url.rfind("data:", 0) == 0) {
        const size_t comma = url.find(',');
        if (comma == std::string::npos) {
            throw std::runtime_error("malformed data: URI (no comma separating the payload)");
        }
        const std::string header = url.substr(0, comma);
        if (header.size() < 6 || header.compare(header.size() - 6, 6, "base64") != 0) {
            throw std::runtime_error("data: URI must be base64 encoded");
        }
        payload = url.substr(comma + 1);
    }

    onyx_media_buffer data = base64_decode(payload);
    if (data.empty()) {
        throw std::runtime_error(std::string("could not decode ") + what + " data (expected base64)");
    }
    return data;
}

size_t json_nesting_depth(const std::string & text) {
    size_t depth = 0;
    size_t deepest = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            depth++;
            deepest = std::max(deepest, depth);
        } else if ((c == ']' || c == '}') && depth > 0) {
            depth--;
        }
    }
    return deepest;
}
