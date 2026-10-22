// Request-level helpers used by the HTTP layer: media payload decoding,
// base64, header checks. Pure functions of their arguments, so they are unit
// tested directly (tests/unit/test_http_util.cpp).
#pragma once

#include <string>
#include <vector>

// Same alias as engine.h: one decoded media file's bytes.
using onyx_media_buffer = std::vector<unsigned char>;

// Base64 decoding as llama-server does it: stops at the first character that
// is neither base64 nor padding.
onyx_media_buffer base64_decode(const std::string & encoded);
// Standard base64 with padding.
std::string base64_encode(const unsigned char * data, size_t n);
// OpenAI's base64 embedding: the float32 vector's bytes, little-endian.
std::string embedding_base64(const std::vector<float> & v);

// Throws std::runtime_error for an http://, https:// or file:// payload:
// onyx-engine never fetches anything on a request's behalf.
void reject_media_url(const std::string & url, const char * what);
// A data: URI or bare base64 to raw bytes; throws std::runtime_error.
onyx_media_buffer decode_media_payload(const std::string & url, const char * what);

// True for an X-Request-Id worth echoing: 1-128 printable ASCII, no spaces.
bool valid_request_id(const std::string & id);
// Equality in time that depends only on the lengths.
bool constant_time_equals(const std::string & a, const std::string & b);
// The host part of a Host header: "[::1]:8080" -> "[::1]", "a:80" -> "a".
std::string host_header_name(const std::string & host);
// True for a dotted-quad IPv4 address in 127.0.0.0/8, and nothing else.
bool onyx_is_ipv4_loopback(const std::string & s);
// True when a Host header names this machine: localhost, *.localhost,
// 127.x.x.x or [::1], with or without a port.
bool host_header_is_loopback(const std::string & host);
