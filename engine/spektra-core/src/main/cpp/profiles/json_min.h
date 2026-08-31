/*
 * Spektrafilm for Android — native engine: minimal dependency-free JSON parser.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm.
 *
 * --------------------------------------------------------------------------------
 * A tiny, header-only, NDK-friendly recursive-descent JSON parser. It supports
 * the subset spektrafilm profile JSON uses: objects, arrays, strings, numbers,
 * booleans, and `null` (decoded as NaN for numeric leaves). No external
 * dependencies; standard library only.
 * --------------------------------------------------------------------------------
 */
#ifndef SPK_PROFILES_JSON_MIN_H
#define SPK_PROFILES_JSON_MIN_H

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace spk {
namespace json {

// V1 hostile-input ceilings. They are deliberately above every bundled
// profile/neutral-filter asset while keeping parser work and allocations
// bounded for untrusted imported content.
inline constexpr size_t kMaxInputBytes = 1u << 20;
inline constexpr size_t kMaxDepth = 8;
inline constexpr size_t kMaxNodes = 16'384;
inline constexpr size_t kMaxArrayElements = 512;
inline constexpr size_t kMaxObjectMembers = 64;
inline constexpr size_t kMaxDecodedStringBytes = 4'096;
inline constexpr size_t kMaxNumberTokenBytes = 128;

class Value;
using ValuePtr = std::shared_ptr<Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;       // NaN for JSON null in numeric context
    bool is_null_number = false;  // true when this leaf was a JSON `null`
    std::string str;
    std::vector<ValuePtr> array;
    std::map<std::string, ValuePtr> object;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }

    // Object member access; throws if missing or wrong type.
    const Value& at(const std::string& key) const {
        if (type != Type::Object) throw std::runtime_error("JSON: not an object for key '" + key + "'");
        auto it = object.find(key);
        if (it == object.end()) throw std::runtime_error("JSON: missing key '" + key + "'");
        return *it->second;
    }
    bool has(const std::string& key) const {
        return type == Type::Object && object.find(key) != object.end();
    }

    const Value& operator[](size_t i) const {
        if (type != Type::Array) throw std::runtime_error("JSON: not an array");
        return *array.at(i);
    }
    size_t size() const { return array.size(); }

    // Numeric leaf -> double; JSON null becomes NaN (matching the Python loader's
    // allow_nan round-trip where missing measurements are stored as `null`).
    double as_number() const {
        if (type == Type::Number) return is_null_number ? std::nan("") : number;
        if (type == Type::Null) return std::nan("");
        throw std::runtime_error("JSON: value is not a number");
    }
    const std::string& as_string() const {
        if (type != Type::String) throw std::runtime_error("JSON: value is not a string");
        return str;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text), n_(text.size()) {}

    ValuePtr parse() {
        if (n_ > kMaxInputBytes)
            throw std::runtime_error("JSON: input exceeds 1 MiB limit");
        skip_ws();
        ValuePtr v = parse_value(/*depth=*/1);
        skip_ws();
        if (pos_ != n_) throw std::runtime_error("JSON: trailing characters");
        return v;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
    size_t n_;
    size_t nodes_ = 0;

    [[noreturn]] void fail(const std::string& msg) {
        throw std::runtime_error("JSON parse error at " + std::to_string(pos_) + ": " + msg);
    }
    char peek() { return pos_ < n_ ? s_[pos_] : '\0'; }
    char get() { return pos_ < n_ ? s_[pos_++] : '\0'; }

    void skip_ws() {
        while (pos_ < n_) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    ValuePtr make_value(Type type) {
        if (nodes_ >= kMaxNodes) fail("node limit exceeded");
        ++nodes_;
        auto value = std::make_shared<Value>();
        value->type = type;
        return value;
    }

    ValuePtr parse_value(size_t depth) {
        if (depth > kMaxDepth) fail("nesting depth limit exceeded");
        // Fail before string/token decoding can allocate when the next value
        // would exceed the global node budget.
        if (nodes_ >= kMaxNodes) fail("node limit exceeded");
        skip_ws();
        char c = peek();
        switch (c) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return parse_string_value();
            case 't': case 'f': return parse_bool();
            case 'n': return parse_null();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
                fail("unexpected token");
        }
    }

    ValuePtr parse_object(size_t depth) {
        auto v = make_value(Type::Object);
        get();  // '{'
        skip_ws();
        if (peek() == '}') { get(); return v; }
        size_t members = 0;
        while (true) {
            // Check before parsing/allocating the next key or child value.
            if (members >= kMaxObjectMembers) fail("object member limit exceeded");
            if (nodes_ >= kMaxNodes) fail("node limit exceeded");
            skip_ws();
            if (peek() != '"') fail("expected string key");
            std::string key = parse_raw_string();
            if (v->object.find(key) != v->object.end())
                fail("duplicate object key");
            skip_ws();
            if (get() != ':') fail("expected ':'");
            ValuePtr val = parse_value(depth + 1);
            v->object.emplace(std::move(key), std::move(val));
            ++members;
            skip_ws();
            char d = get();
            if (d == ',') continue;
            if (d == '}') break;
            fail("expected ',' or '}'");
        }
        return v;
    }

    ValuePtr parse_array(size_t depth) {
        auto v = make_value(Type::Array);
        get();  // '['
        skip_ws();
        if (peek() == ']') { get(); return v; }
        while (true) {
            // Check before allocating the next child and before push_back.
            if (v->array.size() >= kMaxArrayElements)
                fail("array element limit exceeded");
            ValuePtr val = parse_value(depth + 1);
            v->array.push_back(std::move(val));
            skip_ws();
            char d = get();
            if (d == ',') continue;
            if (d == ']') break;
            fail("expected ',' or ']'");
        }
        return v;
    }

    void append_byte(std::string* out, char byte) {
        if (out->size() >= kMaxDecodedStringBytes)
            fail("decoded string limit exceeded");
        out->push_back(byte);
    }

    void append_codepoint(std::string* out, uint32_t codepoint) {
        char encoded[4];
        size_t count = 0;
        if (codepoint <= 0x7f) {
            encoded[count++] = static_cast<char>(codepoint);
        } else if (codepoint <= 0x7ff) {
            encoded[count++] = static_cast<char>(0xc0 | (codepoint >> 6));
            encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
        } else if (codepoint <= 0xffff) {
            encoded[count++] = static_cast<char>(0xe0 | (codepoint >> 12));
            encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
            encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
        } else {
            encoded[count++] = static_cast<char>(0xf0 | (codepoint >> 18));
            encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
            encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
            encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
        }
        if (out->size() > kMaxDecodedStringBytes - count)
            fail("decoded string limit exceeded");
        out->append(encoded, count);
    }

    unsigned hex_digit(char c) {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        fail("bad unicode escape");
    }

    uint32_t parse_hex4() {
        if (n_ - pos_ < 4) fail("bad unicode escape");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 4) | hex_digit(s_[pos_++]);
        return value;
    }

    bool is_continuation(size_t offset) const {
        return pos_ + offset < n_ &&
               (static_cast<unsigned char>(s_[pos_ + offset]) & 0xc0u) == 0x80u;
    }

    void append_raw_utf8(std::string* out, unsigned char lead) {
        const size_t start = pos_ - 1;
        size_t count = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            if (!is_continuation(0)) fail("invalid UTF-8 in string");
            count = 2;
        } else if (lead == 0xe0) {
            if (pos_ + 1 >= n_ || static_cast<unsigned char>(s_[pos_]) < 0xa0 ||
                static_cast<unsigned char>(s_[pos_]) > 0xbf || !is_continuation(1))
                fail("invalid UTF-8 in string");
            count = 3;
        } else if ((lead >= 0xe1 && lead <= 0xec) || (lead >= 0xee && lead <= 0xef)) {
            if (!is_continuation(0) || !is_continuation(1))
                fail("invalid UTF-8 in string");
            count = 3;
        } else if (lead == 0xed) {
            if (pos_ + 1 >= n_ || static_cast<unsigned char>(s_[pos_]) < 0x80 ||
                static_cast<unsigned char>(s_[pos_]) > 0x9f || !is_continuation(1))
                fail("invalid UTF-8 in string");
            count = 3;
        } else if (lead == 0xf0) {
            if (pos_ + 2 >= n_ || static_cast<unsigned char>(s_[pos_]) < 0x90 ||
                static_cast<unsigned char>(s_[pos_]) > 0xbf ||
                !is_continuation(1) || !is_continuation(2))
                fail("invalid UTF-8 in string");
            count = 4;
        } else if (lead >= 0xf1 && lead <= 0xf3) {
            if (!is_continuation(0) || !is_continuation(1) || !is_continuation(2))
                fail("invalid UTF-8 in string");
            count = 4;
        } else if (lead == 0xf4) {
            if (pos_ + 2 >= n_ || static_cast<unsigned char>(s_[pos_]) < 0x80 ||
                static_cast<unsigned char>(s_[pos_]) > 0x8f ||
                !is_continuation(1) || !is_continuation(2))
                fail("invalid UTF-8 in string");
            count = 4;
        } else {
            fail("invalid UTF-8 in string");
        }
        if (out->size() > kMaxDecodedStringBytes - count)
            fail("decoded string limit exceeded");
        out->append(s_, start, count);
        pos_ += count - 1;
    }

    std::string parse_raw_string() {
        get();  // opening quote
        std::string out;
        while (pos_ < n_) {
            const unsigned char byte = static_cast<unsigned char>(s_[pos_++]);
            char c = static_cast<char>(byte);
            if (c == '"') return out;
            if (c == '\\') {
                char e = pos_ < n_ ? s_[pos_++] : '\0';
                switch (e) {
                    case '"': append_byte(&out, '"'); break;
                    case '\\': append_byte(&out, '\\'); break;
                    case '/': append_byte(&out, '/'); break;
                    case 'b': append_byte(&out, '\b'); break;
                    case 'f': append_byte(&out, '\f'); break;
                    case 'n': append_byte(&out, '\n'); break;
                    case 'r': append_byte(&out, '\r'); break;
                    case 't': append_byte(&out, '\t'); break;
                    case 'u': {
                        uint32_t code = parse_hex4();
                        if (code >= 0xd800 && code <= 0xdbff) {
                            if (n_ - pos_ < 6 || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                                fail("unpaired high surrogate");
                            pos_ += 2;
                            const uint32_t low = parse_hex4();
                            if (low < 0xdc00 || low > 0xdfff)
                                fail("unpaired high surrogate");
                            code = 0x10000u + ((code - 0xd800u) << 10) +
                                   (low - 0xdc00u);
                        } else if (code >= 0xdc00 && code <= 0xdfff) {
                            fail("unpaired low surrogate");
                        }
                        append_codepoint(&out, code);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                if (byte < 0x20) fail("unescaped control character in string");
                if (byte < 0x80) append_byte(&out, c);
                else append_raw_utf8(&out, byte);
            }
        }
        fail("unterminated string");
    }

    ValuePtr parse_string_value() {
        std::string decoded = parse_raw_string();
        auto v = make_value(Type::String);
        v->str = std::move(decoded);
        return v;
    }

    ValuePtr parse_bool() {
        bool boolean = false;
        if (s_.compare(pos_, 4, "true") == 0) { boolean = true; pos_ += 4; }
        else if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; }
        else fail("invalid literal");
        auto v = make_value(Type::Bool);
        v->boolean = boolean;
        return v;
    }

    ValuePtr parse_null() {
        if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; }
        else fail("invalid literal");
        // Represent null as a Number leaf carrying NaN so numeric arrays parse
        // uniformly; as_number() yields NaN either way.
        auto v = make_value(Type::Number);
        v->is_null_number = true;
        v->number = std::nan("");
        return v;
    }

    ValuePtr parse_number() {
        size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (peek() == '0') {
            ++pos_;
            if (peek() >= '0' && peek() <= '9') fail("leading zero in number");
        } else if (peek() >= '1' && peek() <= '9') {
            do { ++pos_; } while (peek() >= '0' && peek() <= '9');
        } else {
            fail("invalid number");
        }
        if (peek() == '.') {
            ++pos_;
            if (peek() < '0' || peek() > '9') fail("missing fractional digit");
            do { ++pos_; } while (peek() >= '0' && peek() <= '9');
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (peek() < '0' || peek() > '9') fail("missing exponent digit");
            do { ++pos_; } while (peek() >= '0' && peek() <= '9');
        }
        const size_t token_size = pos_ - start;
        if (token_size > kMaxNumberTokenBytes) fail("number token limit exceeded");
        const std::string token = s_.substr(start, token_size);
        char* end = nullptr;
        errno = 0;
        const double number = std::strtod(token.c_str(), &end);
        if (errno == ERANGE || end != token.c_str() + token.size() ||
            !std::isfinite(number))
            fail("number is out of range");
        auto v = make_value(Type::Number);
        v->number = number;
        return v;
    }
};

inline ValuePtr parse(const std::string& text) {
    return Parser(text).parse();
}

}  // namespace json
}  // namespace spk

#endif  // SPK_PROFILES_JSON_MIN_H
