// Request validation helpers from the HTTP layer.
#include "check.h"

#include "http_util.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string bytes_str(const onyx_media_buffer & b) {
    return std::string(b.begin(), b.end());
}

} // namespace

TEST(http, base64_round_trip) {
    const std::string samples[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const std::string encoded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (size_t i = 0; i < 7; i++) {
        const auto & s = samples[i];
        CHECK_EQ(base64_encode((const unsigned char *) s.data(), s.size()), encoded[i]);
        CHECK_EQ(bytes_str(base64_decode(encoded[i])), s);
    }
}

TEST(http, base64_decode_stops_at_garbage) {
    CHECK_EQ(bytes_str(base64_decode("Zm9v!!!!Zm9v")), std::string("foo"));
    CHECK_EQ(bytes_str(base64_decode("Zm9v\nYmFy")), std::string("foo"));
}

TEST(http, embedding_base64_is_little_endian_float32) {
    const std::vector<float> v = {1.0f, -2.5f};
    const onyx_media_buffer raw = base64_decode(embedding_base64(v));
    CHECK_EQ(raw.size(), (size_t) 8);
    // 1.0f = 0x3f800000, -2.5f = 0xc0200000, least significant byte first
    const unsigned char want[8] = {0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x20, 0xc0};
    CHECK(memcmp(raw.data(), want, 8) == 0);
}

TEST(http, media_urls_are_refused) {
    CHECK_THROWS(reject_media_url("http://example.com/cat.png", "image"));
    CHECK_THROWS(reject_media_url("https://example.com/cat.png", "image"));
    CHECK_THROWS(reject_media_url("file:///etc/passwd", "image"));
    reject_media_url("data:image/png;base64,iVBORw0KGgo=", "image");   // must not throw
    reject_media_url("iVBORw0KGgo=", "image");
}

TEST(http, data_uris) {
    CHECK_EQ(bytes_str(decode_media_payload("data:text/plain;base64,Zm9v", "image")), std::string("foo"));
    CHECK_EQ(bytes_str(decode_media_payload("Zm9vYmFy", "audio")), std::string("foobar"));
    CHECK_THROWS(decode_media_payload("data:text/plain;base64", "image"));      // no comma
    CHECK_THROWS(decode_media_payload("data:text/plain,Zm9v", "image"));        // not base64
    CHECK_THROWS(decode_media_payload("!!!!", "image"));                         // decodes to nothing
}

TEST(http, request_ids) {
    CHECK(valid_request_id("req-123.abc"));
    CHECK(valid_request_id(std::string(128, 'a')));
    CHECK(!valid_request_id(std::string(129, 'a')));
    CHECK(!valid_request_id(""));
    CHECK(!valid_request_id("a b"));
    CHECK(!valid_request_id("a\r\nX-Injected: 1"));
    CHECK(!valid_request_id("caf\xc3\xa9"));
}

TEST(http, constant_time_equals) {
    CHECK(constant_time_equals("Bearer sk-1", "Bearer sk-1"));
    CHECK(constant_time_equals("", ""));
    CHECK(!constant_time_equals("Bearer sk-1", "Bearer sk-2"));
    CHECK(!constant_time_equals("Bearer sk-1", "Bearer sk-1x"));
    CHECK(!constant_time_equals("Bearer sk-1", "Bearer sk-"));
    CHECK(!constant_time_equals("", "x"));
    // a shorter string that is a zero-padded prefix must not compare equal
    CHECK(!constant_time_equals(std::string("ab\0", 3), "ab"));
}

TEST(http, ipv4_loopback) {
    CHECK(onyx_is_ipv4_loopback("127.0.0.1"));
    CHECK(onyx_is_ipv4_loopback("127.255.255.255"));
    CHECK(!onyx_is_ipv4_loopback("127.0.0"));
    CHECK(!onyx_is_ipv4_loopback("127.0.0.1.2"));
    CHECK(!onyx_is_ipv4_loopback("127.0.0.256"));
    CHECK(!onyx_is_ipv4_loopback("127.0.0.1."));
    CHECK(!onyx_is_ipv4_loopback("127..0.1"));
    CHECK(!onyx_is_ipv4_loopback("127.0.0.1x"));
    CHECK(!onyx_is_ipv4_loopback("127.0.0.0001"));
    CHECK(!onyx_is_ipv4_loopback("128.0.0.1"));
    CHECK(!onyx_is_ipv4_loopback("127.0.0.1.evil.example"));
    CHECK(!onyx_is_ipv4_loopback(""));
}

TEST(http, host_header_names) {
    CHECK_EQ(host_header_name("localhost:8080"), std::string("localhost"));
    CHECK_EQ(host_header_name("[::1]:8080"), std::string("[::1]"));
    CHECK_EQ(host_header_name("[::1]"), std::string("[::1]"));
    CHECK_EQ(host_header_name("127.0.0.1"), std::string("127.0.0.1"));
}

TEST(http, loopback_host_headers) {
    CHECK(host_header_is_loopback("127.0.0.1:8080"));
    CHECK(host_header_is_loopback("127.9.9.9"));
    CHECK(host_header_is_loopback("localhost"));
    CHECK(host_header_is_loopback("LOCALHOST:1"));
    CHECK(host_header_is_loopback("app.localhost:3000"));
    CHECK(host_header_is_loopback("[::1]:8080"));
    CHECK(!host_header_is_loopback("evil.example"));
    CHECK(!host_header_is_loopback("evil.example:8080"));
    CHECK(!host_header_is_loopback("localhost.evil.example"));
    CHECK(!host_header_is_loopback("127.0.0.1.evil.example"));
    CHECK(!host_header_is_loopback("host.docker.internal:8080"));
    CHECK(!host_header_is_loopback("192.168.1.2"));
    CHECK(!host_header_is_loopback(""));
}
