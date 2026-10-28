// KZ3 net: strict, bounded JSON for Nakama traffic (every byte from a server is untrusted).
//
// The parser rejects, instead of guessing: input over the byte cap, nesting over the depth cap, more nodes than
// the node cap, strings over the string cap, invalid UTF-8, raw control characters, lone surrogates, duplicate
// object keys, trailing garbage, leading zeros, NaN/Infinity. Numbers keep their source text so 64-bit integers
// (Nakama sends int64 as strings or numbers) are parsed exactly by the caller, never through a double.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kz3net {

struct JsonLimits {
    size_t max_bytes = 64 * 1024;   // whole document (matches the Nakama socket.max_message_size_bytes we ask for)
    size_t max_depth = 16;
    size_t max_nodes = 4096;
    size_t max_string = 64 * 1024; // one decoded string
};

class Json {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json null() { return Json(); }
    static Json boolean(bool b) { Json j; j.type_ = Type::Bool; j.b_ = b; return j; }
    static Json number_text(std::string t) { Json j; j.type_ = Type::Number; j.s_ = std::move(t); return j; }
    static Json integer(int64_t v) { return number_text(std::to_string(v)); }
    static Json string(std::string s) { Json j; j.type_ = Type::String; j.s_ = std::move(s); return j; }
    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const { return b_; }
    const std::string& str() const { return s_; }          // String value, or the Number's source text
    const std::vector<Json>& items() const { return arr_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }

    // Object lookup; nullptr when absent or when this is not an object.
    const Json* get(const char* key) const;
    // Typed lookups: false when absent or of the wrong type (absent and wrong type are both "not usable").
    bool get_string(const char* key, std::string& out, size_t max_len) const;
    bool get_bool(const char* key, bool& out) const;
    // Integer from a JSON number or a decimal string ("123", "-4"), exact, range-checked. No fractions/exponents.
    bool get_int64(const char* key, int64_t& out, int64_t lo, int64_t hi) const;

    // Builders (no limits: we only build what we send).
    Json& push(Json v) { arr_.push_back(std::move(v)); return arr_.back(); }
    Json& set(std::string key, Json v);

    std::string dump() const;

private:
    Type type_ = Type::Null;
    bool b_ = false;
    std::string s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
    friend class JsonParser;
};

// Parses `text` into `out`. On failure returns false and sets `err` (static string, for logs).
bool json_parse(const std::string& text, Json& out, const char** err, const JsonLimits& lim = JsonLimits());

// Exact decimal int64 parse of a whole string ("-0" ok, no '+', no leading zeros, no spaces).
bool parse_int64_text(const std::string& s, int64_t& out);

void json_escape_to(std::string& out, const std::string& s);

} // namespace kz3net
