// KZ3 net: strict bounded JSON (see json.h).
#include "json.h"

#include <cstring>

namespace kz3net {

const Json* Json::get(const char* key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& m : obj_)
        if (m.first == key) return &m.second;
    return nullptr;
}

bool Json::get_string(const char* key, std::string& out, size_t max_len) const {
    const Json* v = get(key);
    if (!v || !v->is_string() || v->s_.size() > max_len) return false;
    out = v->s_;
    return true;
}

bool Json::get_bool(const char* key, bool& out) const {
    const Json* v = get(key);
    if (!v || !v->is_bool()) return false;
    out = v->b_;
    return true;
}

bool parse_int64_text(const std::string& s, int64_t& out) {
    size_t i = 0, n = s.size();
    bool neg = false;
    if (n == 0 || n > 20) return false;
    if (s[0] == '-') { neg = true; i = 1; }
    if (i >= n) return false;
    if (s[i] == '0' && i + 1 < n) return false;  // leading zero
    uint64_t v = 0;
    const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    for (; i < n; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        unsigned d = (unsigned)(c - '0');
        if (v > (limit - d) / 10u) return false;
        v = v * 10u + d;
    }
    out = neg ? (int64_t)(0u - v) : (int64_t)v;
    return true;
}

bool Json::get_int64(const char* key, int64_t& out, int64_t lo, int64_t hi) const {
    const Json* v = get(key);
    if (!v || !(v->is_number() || v->is_string())) return false;
    int64_t x;
    if (!parse_int64_text(v->s_, x) || x < lo || x > hi) return false;
    out = x;
    return true;
}

Json& Json::set(std::string key, Json v) {
    for (auto& m : obj_)
        if (m.first == key) { m.second = std::move(v); return m.second; }
    obj_.emplace_back(std::move(key), std::move(v));
    return obj_.back().second;
}

void json_escape_to(std::string& out, const std::string& s) {
    static const char hex[] = "0123456789abcdef";
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7F) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += (char)c;
        }
    }
    out += '"';
}

static void dump_to(const Json& j, std::string& out) {
    switch (j.type()) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += j.as_bool() ? "true" : "false"; break;
    case Json::Type::Number: out += j.str(); break;
    case Json::Type::String: json_escape_to(out, j.str()); break;
    case Json::Type::Array: {
        out += '[';
        bool first = true;
        for (const auto& e : j.items()) { if (!first) out += ','; first = false; dump_to(e, out); }
        out += ']';
        break;
    }
    case Json::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& m : j.members()) {
            if (!first) out += ',';
            first = false;
            json_escape_to(out, m.first);
            out += ':';
            dump_to(m.second, out);
        }
        out += '}';
        break;
    }
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(*this, out);
    return out;
}

class JsonParser {
public:
    JsonParser(const std::string& t, const JsonLimits& l) : p_(t.data()), end_(t.data() + t.size()), lim_(l) {}
    const char* err = nullptr;

    bool document(Json& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        if (p_ != end_) return fail("trailing data");
        return true;
    }

private:
    const char* p_;
    const char* end_;
    const JsonLimits& lim_;
    size_t nodes_ = 0;

    bool fail(const char* e) { if (!err) err = e; return false; }
    void ws() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++; }
    bool lit(const char* s) {
        size_t n = std::strlen(s);
        if ((size_t)(end_ - p_) < n || std::memcmp(p_, s, n) != 0) return fail("bad literal");
        p_ += n;
        return true;
    }

    bool value(Json& out, size_t depth) {
        if (++nodes_ > lim_.max_nodes) return fail("too many nodes");
        if (p_ >= end_) return fail("unexpected end");
        switch (*p_) {
        case 'n': out = Json(); return lit("null");
        case 't': out = Json::boolean(true); return lit("true");
        case 'f': out = Json::boolean(false); return lit("false");
        case '"': out.type_ = Json::Type::String; return string(out.s_);
        case '[': return array(out, depth);
        case '{': return object(out, depth);
        default: return number(out);
        }
    }

    bool number(Json& out) {
        const char* s = p_;
        if (p_ < end_ && *p_ == '-') p_++;
        if (p_ >= end_) return fail("bad number");
        if (*p_ == '0') p_++;
        else if (*p_ >= '1' && *p_ <= '9') { while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++; }
        else return fail("bad value");
        if (p_ < end_ && *p_ == '.') {
            p_++;
            if (p_ >= end_ || *p_ < '0' || *p_ > '9') return fail("bad fraction");
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            p_++;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) p_++;
            if (p_ >= end_ || *p_ < '0' || *p_ > '9') return fail("bad exponent");
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        }
        if ((size_t)(p_ - s) > 64) return fail("number too long");
        out = Json::number_text(std::string(s, p_));
        return true;
    }

    static void put_utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& v) {
        if (end_ - p_ < 4) return fail("bad \\u escape");
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    // One UTF-8 sequence, validated (no overlongs, no surrogates, <= U+10FFFF).
    bool utf8(std::string& o) {
        unsigned char c = (unsigned char)*p_;
        int n;
        uint32_t cp, min;
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; min = 0x10000; }
        else return fail("invalid utf-8");
        if (end_ - p_ < n + 1) return fail("invalid utf-8");
        for (int i = 1; i <= n; i++) {
            unsigned char d = (unsigned char)p_[i];
            if ((d & 0xC0) != 0x80) return fail("invalid utf-8");
            cp = (cp << 6) | (d & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("invalid utf-8");
        o.append(p_, (size_t)n + 1);
        p_ += n + 1;
        return true;
    }

    bool string(std::string& o) {
        p_++;  // opening quote
        o.clear();
        for (;;) {
            if (p_ >= end_) return fail("unterminated string");
            if (o.size() > lim_.max_string) return fail("string too long");
            unsigned char c = (unsigned char)*p_;
            if (c == '"') { p_++; break; }
            if (c < 0x20) return fail("control char in string");
            if (c == '\\') {
                p_++;
                if (p_ >= end_) return fail("bad escape");
                char e = *p_++;
                switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo;
                        if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') return fail("lone high surrogate");
                        p_ += 2;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(o, cp);
                    break;
                }
                default: return fail("bad escape");
                }
            } else if (c < 0x80) {
                o += (char)c;
                p_++;
            } else if (!utf8(o)) {
                return false;
            }
        }
        if (o.size() > lim_.max_string) return fail("string too long");
        return true;
    }

    bool array(Json& out, size_t depth) {
        if (depth + 1 > lim_.max_depth) return fail("too deep");
        out = Json::array();
        p_++;
        ws();
        if (p_ < end_ && *p_ == ']') { p_++; return true; }
        for (;;) {
            ws();
            out.arr_.emplace_back();
            if (!value(out.arr_.back(), depth + 1)) return false;
            ws();
            if (p_ >= end_) return fail("unterminated array");
            if (*p_ == ',') { p_++; continue; }
            if (*p_ == ']') { p_++; return true; }
            return fail("expected , or ]");
        }
    }

    bool object(Json& out, size_t depth) {
        if (depth + 1 > lim_.max_depth) return fail("too deep");
        out = Json::object();
        p_++;
        ws();
        if (p_ < end_ && *p_ == '}') { p_++; return true; }
        for (;;) {
            ws();
            if (p_ >= end_ || *p_ != '"') return fail("expected key");
            std::string key;
            if (!string(key)) return false;
            for (const auto& m : out.obj_)
                if (m.first == key) return fail("duplicate key");
            ws();
            if (p_ >= end_ || *p_ != ':') return fail("expected :");
            p_++;
            ws();
            out.obj_.emplace_back(std::move(key), Json());
            if (!value(out.obj_.back().second, depth + 1)) return false;
            ws();
            if (p_ >= end_) return fail("unterminated object");
            if (*p_ == ',') { p_++; continue; }
            if (*p_ == '}') { p_++; return true; }
            return fail("expected , or }");
        }
    }
};

bool json_parse(const std::string& text, Json& out, const char** err, const JsonLimits& lim) {
    if (err) *err = nullptr;
    if (text.size() > lim.max_bytes) { if (err) *err = "document too large"; return false; }
    JsonParser p(text, lim);
    Json tmp;
    if (!p.document(tmp)) { if (err) *err = p.err ? p.err : "parse error"; return false; }
    out = std::move(tmp);
    return true;
}

} // namespace kz3net
