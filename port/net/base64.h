// KZ3 net: base64 (RFC 4648) for Nakama match data and Basic auth, base64url for JWT payloads.
// Decoding is strict (canonical padding, no whitespace, no stray bits) and bounded by max_out.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kz3net {

std::string base64_encode(const uint8_t* data, size_t len);
inline std::string base64_encode(const std::string& s) { return base64_encode((const uint8_t*)s.data(), s.size()); }

// Standard alphabet, padding required. False on any malformed input or when the result would exceed max_out.
bool base64_decode(const std::string& in, std::vector<uint8_t>& out, size_t max_out);
// URL-safe alphabet, padding optional (JWT segments).
bool base64url_decode(const std::string& in, std::vector<uint8_t>& out, size_t max_out);

} // namespace kz3net
