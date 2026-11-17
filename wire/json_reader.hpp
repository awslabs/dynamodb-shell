// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// json_reader.hpp — a compact, SDK-free JSON reader internal to ddbsh-wire
// (P6b). The P6a codec ships a bespoke JSON *writer* (it must reproduce the AWS
// SDK's cJSON byte formatting); the response parser needs the inverse — turn a
// response body string into a value tree it can walk. This is that reader.
//
// WHY HAND-WRITTEN (D-P6b-1). A third-party JSON library (nlohmann, RapidJSON)
// would add a dependency and WASM binary weight for a job this small, and P6a
// already committed to a hand-written emitter for the same reasons (P6-02 O-5).
// A ~200-line reader keeps ddbsh-wire dependency-free and lean on every
// platform. A vendored single-header reader is a drop-in behind this same
// interface if ever preferred.
//
// SDK-FREE + TERMINAL-FREE. This file lives under wire/, which the engine
// boundary check (tests/boundary_check.sh) scans: no <iostream>/cout/cerr/
// printf/FILE*/exit/abort. Parse failures are carried by value in a
// ddbsh::Error (Result<T>), never thrown or exited.
//
// NUMBER FIDELITY. Numbers are carried as their VERBATIM source substring
// (JsonValue::text for a Number). AWS JSON 1.0 carries DynamoDB numeric
// AttributeValues as JSON *strings* ({"N":"123.45"}), so N fidelity is free (it
// is a JSON string, not a JSON number). The only JSON *numbers* the six-op
// responses carry are ConsumedCapacity's doubles; the caller converts those
// token strings to double with std::from_chars at the point of use. No number
// ever round-trips through a float on the AttributeValue path.
//
// OBJECT KEY ORDER is preserved as encountered (a vector<pair> DOM), so the
// parser can feed a ValueMap in response order (the Renderer sorts item keys at
// format time, arch §9, so object order does not affect output).

#ifndef DDBSH_WIRE_JSON_READER_HPP
#define DDBSH_WIRE_JSON_READER_HPP

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ddbsh/result.hpp"

namespace ddbsh::wire::json {

/// A minimal JSON value tree. `Number` is carried as its verbatim token text
/// (never parsed to a float by the reader); `String` is fully unescaped.
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;             ///< Bool
    std::string text;                 ///< String (decoded) or Number (verbatim token)
    std::vector<JsonValue> elements;  ///< Array, in order
    std::vector<std::pair<std::string, JsonValue>> members;  ///< Object, in order

    [[nodiscard]] bool is_object() const { return kind == Kind::Object; }
    [[nodiscard]] bool is_array() const { return kind == Kind::Array; }
    [[nodiscard]] bool is_string() const { return kind == Kind::String; }
    [[nodiscard]] bool is_number() const { return kind == Kind::Number; }
    [[nodiscard]] bool is_bool() const { return kind == Kind::Bool; }

    /// First member whose key equals `key`, or nullptr. DynamoDB response
    /// objects never carry duplicate keys; first-match is sufficient.
    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        for (const auto& [k, v] : members) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

/// Parse a complete JSON document from `body`. Returns the root value, or a
/// ddbsh::Error (Errc::service_error) on any malformed input — a response the
/// engine cannot parse is a service-level failure from the caller's view. The
/// whole input must be consumed (trailing non-whitespace is an error).
Result<JsonValue> parse(std::string_view body);

}  // namespace ddbsh::wire::json

#endif  // DDBSH_WIRE_JSON_READER_HPP
