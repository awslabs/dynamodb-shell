// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// json_reader.cpp — the SDK-free JSON reader (P6b; see json_reader.hpp).
//
// A hand-written recursive-descent parser producing a minimal ordered DOM.
// Strings are decoded (the inverse of P6a write_string): the named escapes
// cJSON emits (\" \\ \/ \b \f \n \r \t), \uXXXX (with surrogate-pair handling),
// and literal bytes (incl. UTF-8 continuation) copied verbatim. Numbers are
// kept as their verbatim token text. A bounded recursion depth guards against a
// pathologically nested payload overflowing the stack. Every failure path
// returns an Error (Errc::service_error) with a "malformed response: ..."
// message — the reader never throws and never exits (the wire boundary rule).

#include "wire/json_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ddbsh::wire::json {
namespace {

// A cap deep enough for any real DynamoDB payload (document attributes nest
// shallowly) but shallow enough that a hostile input cannot overflow the stack.
constexpr int kMaxDepth = 256;

std::unexpected<Error> malformed(const std::string& detail) {
    return fail(Errc::service_error, "malformed response: " + detail);
}

/// A single-pass cursor over the input. `pos` always points at the next byte to
/// consume. Whitespace handling and the recursive value grammar live here.
class Parser {
public:
    explicit Parser(std::string_view in) : s_(in) {}

    Result<JsonValue> parse_document() {
        skip_ws();
        auto v = parse_value(0);
        if (!v) {
            return v;
        }
        skip_ws();
        if (pos_ != s_.size()) {
            return malformed("trailing characters after value");
        }
        return v;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;

    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }

    void skip_ws() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    Result<JsonValue> parse_value(int depth) {
        if (depth > kMaxDepth) {
            return malformed("nesting too deep");
        }
        if (eof()) {
            return malformed("unexpected end of input");
        }
        const char c = peek();
        switch (c) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                std::string out;
                if (auto err = parse_string(out)) {
                    return fail(*err);
                }
                JsonValue v;
                v.kind = JsonValue::Kind::String;
                v.text = std::move(out);
                return v;
            }
            case 't':
            case 'f':
                return parse_bool();
            case 'n':
                return parse_null();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parse_number();
                }
                return malformed("unexpected token");
        }
    }

    Result<JsonValue> parse_object(int depth) {
        ++pos_;  // consume '{'
        JsonValue obj;
        obj.kind = JsonValue::Kind::Object;
        skip_ws();
        if (!eof() && peek() == '}') {
            ++pos_;
            return obj;
        }
        for (;;) {
            skip_ws();
            if (eof() || peek() != '"') {
                return malformed("expected object key string");
            }
            std::string key;
            if (auto err = parse_string(key)) {
                return fail(*err);
            }
            skip_ws();
            if (eof() || peek() != ':') {
                return malformed("expected ':' after object key");
            }
            ++pos_;  // consume ':'
            skip_ws();
            auto val = parse_value(depth + 1);
            if (!val) {
                return val;
            }
            obj.members.emplace_back(std::move(key), std::move(*val));
            skip_ws();
            if (eof()) {
                return malformed("unterminated object");
            }
            const char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                return obj;
            }
            return malformed("expected ',' or '}' in object");
        }
    }

    Result<JsonValue> parse_array(int depth) {
        ++pos_;  // consume '['
        JsonValue arr;
        arr.kind = JsonValue::Kind::Array;
        skip_ws();
        if (!eof() && peek() == ']') {
            ++pos_;
            return arr;
        }
        for (;;) {
            skip_ws();
            auto val = parse_value(depth + 1);
            if (!val) {
                return val;
            }
            arr.elements.push_back(std::move(*val));
            skip_ws();
            if (eof()) {
                return malformed("unterminated array");
            }
            const char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == ']') {
                ++pos_;
                return arr;
            }
            return malformed("expected ',' or ']' in array");
        }
    }

    Result<JsonValue> parse_bool() {
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            JsonValue v;
            v.kind = JsonValue::Kind::Bool;
            v.boolean = true;
            return v;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            JsonValue v;
            v.kind = JsonValue::Kind::Bool;
            v.boolean = false;
            return v;
        }
        return malformed("invalid literal");
    }

    Result<JsonValue> parse_null() {
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            JsonValue v;
            v.kind = JsonValue::Kind::Null;
            return v;
        }
        return malformed("invalid literal");
    }

    // Keeps the verbatim number token (no float conversion in the reader). The
    // grammar is JSON's: optional '-', integer part, optional fraction,
    // optional exponent.
    Result<JsonValue> parse_number() {
        const std::size_t start = pos_;
        if (!eof() && peek() == '-') {
            ++pos_;
        }
        if (eof() || !is_digit(peek())) {
            return malformed("invalid number");
        }
        if (peek() == '0') {
            ++pos_;  // a leading zero stands alone
        } else {
            while (!eof() && is_digit(peek())) {
                ++pos_;
            }
        }
        if (!eof() && peek() == '.') {
            ++pos_;
            if (eof() || !is_digit(peek())) {
                return malformed("invalid number fraction");
            }
            while (!eof() && is_digit(peek())) {
                ++pos_;
            }
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            if (eof() || !is_digit(peek())) {
                return malformed("invalid number exponent");
            }
            while (!eof() && is_digit(peek())) {
                ++pos_;
            }
        }
        JsonValue v;
        v.kind = JsonValue::Kind::Number;
        v.text = std::string(s_.substr(start, pos_ - start));
        return v;
    }

    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    // Append the UTF-8 encoding of a Unicode code point to `out`.
    static void append_utf8(std::uint32_t cp, std::string& out) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // Read a \uXXXX escape body (the four hex digits after "\u") into `cp`.
    // `pos_` must point just past the 'u'. Advances past the four digits.
    Result<std::uint32_t> read_hex4() {
        if (pos_ + 4 > s_.size()) {
            return malformed("truncated \\u escape");
        }
        std::uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex_value(s_[pos_ + static_cast<std::size_t>(i)]);
            if (h < 0) {
                return malformed("invalid \\u escape");
            }
            cp = (cp << 4) | static_cast<std::uint32_t>(h);
        }
        pos_ += 4;
        return cp;
    }

    // Parse a JSON string literal (opening quote at pos_) and decode it into
    // `out`. Returns an Error on a malformed escape / unterminated string.
    std::optional<Error> parse_string(std::string& out) {
        ++pos_;  // consume opening '"'
        while (!eof()) {
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return std::nullopt;
            }
            if (c == '\\') {
                ++pos_;
                if (eof()) {
                    return malformed("unterminated escape").error();
                }
                const char e = s_[pos_];
                ++pos_;
                switch (e) {
                    case '"':
                        out += '"';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case '/':
                        out += '/';
                        break;
                    case 'b':
                        out += '\b';
                        break;
                    case 'f':
                        out += '\f';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'u': {
                        auto hi = read_hex4();
                        if (!hi) {
                            return hi.error();
                        }
                        std::uint32_t cp = *hi;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // High surrogate: a low surrogate \uDC00-\uDFFF must
                            // follow to form a valid pair.
                            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
                                return malformed("unpaired high surrogate").error();
                            }
                            pos_ += 2;  // consume "\u"
                            auto lo = read_hex4();
                            if (!lo) {
                                return lo.error();
                            }
                            if (*lo < 0xDC00 || *lo > 0xDFFF) {
                                return malformed("invalid low surrogate").error();
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (*lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return malformed("unexpected low surrogate").error();
                        }
                        append_utf8(cp, out);
                        break;
                    }
                    default:
                        return malformed("invalid escape character").error();
                }
                continue;
            }
            if (c < 0x20) {
                return malformed("unescaped control character in string").error();
            }
            out += static_cast<char>(c);
            ++pos_;
        }
        return malformed("unterminated string").error();
    }
};

}  // namespace

Result<JsonValue> parse(std::string_view body) {
    Parser p(body);
    return p.parse_document();
}

}  // namespace ddbsh::wire::json
