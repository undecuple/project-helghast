// KZ3 net: base64 (see base64.h).
#include "base64.h"

namespace kz3net {

static const char kStd[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* d, size_t n) {
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        o += kStd[v >> 18]; o += kStd[(v >> 12) & 63]; o += kStd[(v >> 6) & 63]; o += kStd[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)d[i] << 16;
        o += kStd[v >> 18]; o += kStd[(v >> 12) & 63]; o += "==";
    } else if (n - i == 2) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8;
        o += kStd[v >> 18]; o += kStd[(v >> 12) & 63]; o += kStd[(v >> 6) & 63]; o += '=';
    }
    return o;
}

static int val(char c, bool url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (!url && c == '+') return 62;
    if (!url && c == '/') return 63;
    if (url && c == '-') return 62;
    if (url && c == '_') return 63;
    return -1;
}

static bool decode(const std::string& in, std::vector<uint8_t>& out, size_t max_out, bool url) {
    size_t n = in.size();
    size_t pad = 0;
    if (!url) {
        if (n % 4 != 0) return false;
        if (n >= 1 && in[n - 1] == '=') pad++;
        if (n >= 2 && in[n - 2] == '=') pad++;
    } else {
        while (n > 0 && in[n - 1] == '=' && pad < 2) { n--; pad++; }
        if (pad && (n + pad) % 4 != 0) return false;
        pad = 0;
        if (n % 4 == 1) return false;
    }
    size_t body = n - pad;            // significant characters
    size_t outlen = body / 4 * 3 + (body % 4 ? body % 4 - 1 : 0);
    if (outlen > max_out) return false;
    std::vector<uint8_t> o;
    o.reserve(outlen);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < body; i++) {
        int v = val(in[i], url);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((uint8_t)(acc >> bits)); acc &= (1u << bits) - 1; }
    }
    if (acc != 0) return false;        // non-canonical: leftover bits must be zero
    out = std::move(o);
    return true;
}

bool base64_decode(const std::string& in, std::vector<uint8_t>& out, size_t max_out) {
    return decode(in, out, max_out, false);
}
bool base64url_decode(const std::string& in, std::vector<uint8_t>& out, size_t max_out) {
    return decode(in, out, max_out, true);
}

} // namespace kz3net
