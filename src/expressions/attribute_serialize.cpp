// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// attribute_serialize.cpp — see attribute_serialize.hpp.

#include "expressions/attribute_serialize.hpp"

#include <algorithm>

namespace ddbsh::engine {

namespace {

// Append the JSON-escaped form of `in` to `out` (quotes, backslashes, control
// characters). Mirrors the minimal escaping the SDK's JSON writer performs.
void append_escaped(std::string& out, const std::string& in) {
    for (char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\n':
                // SerializeAttribute output is newline-stripped downstream; a
                // literal newline inside a string is escaped here.
                out += "\\n";
                break;
            default:
                out += c;
                break;
        }
    }
}

std::string serialize_impl(const Value& value) {
    switch (value.type) {
        case Value::Type::S: {
            std::string out = "{\"S\":\"";
            append_escaped(out, value.s);
            out += "\"}";
            return out;
        }
        case Value::Type::N:
            return "{\"N\":\"" + value.s + "\"}";
        case Value::Type::B: {
            std::string out = "{\"B\":\"";
            append_escaped(out, value.s);
            out += "\"}";
            return out;
        }
        case Value::Type::BOOL:
            return value.boolean ? "{\"BOOL\":true}" : "{\"BOOL\":false}";
        case Value::Type::NUL:
            return "{\"NULL\":true}";
        case Value::Type::L: {
            std::string out = "{\"L\":[";
            bool first = true;
            for (const auto& element : value.list) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += serialize_impl(element);
            }
            out += "]}";
            return out;
        }
        case Value::Type::M: {
            std::string out = "{\"M\":{";
            bool first = true;
            for (const auto& [key, element] : value.map) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += "\"";
                append_escaped(out, key);
                out += "\":";
                out += serialize_impl(element);
            }
            out += "}}";
            return out;
        }
        case Value::Type::SS:
        case Value::Type::NS:
        case Value::Type::BS: {
            const char* tag = value.type == Value::Type::SS   ? "\"SS\""
                              : value.type == Value::Type::NS ? "\"NS\""
                                                              : "\"BS\"";
            std::string out = "{";
            out += tag;
            out += ":[";
            bool first = true;
            for (const auto& element : value.set) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += "\"";
                append_escaped(out, element);
                out += "\"";
            }
            out += "]}";
            return out;
        }
    }
    return "{}";
}

}  // namespace

std::string serialize_attribute(const Value& value) {
    std::string out = serialize_impl(value);
    out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
    return out;
}

}  // namespace ddbsh::engine
