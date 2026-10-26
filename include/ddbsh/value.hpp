// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// value.hpp — the library's SDK-free DynamoDB attribute-value mirror.
//
// D11: public headers must not reference Aws:: types. The engine carries its
// own recursive Value so the Renderer can format results (and the parser can
// build request items) without the AWS SDK. The CLI-side AwsDDBClient adapter
// is the sole place Aws::DynamoDB::Model::AttributeValue <-> ddbsh::Value
// translation happens.
//
// Fidelity note (P2a-03 §3): numeric values (N) are stored as their EXACT
// string, never parsed to double, preserving DynamoDB arbitrary numeric
// precision and the original's bare-number byte output.

#ifndef DDBSH_VALUE_HPP
#define DDBSH_VALUE_HPP

#include <string>
#include <utility>
#include <vector>

namespace ddbsh {

struct Value;

/// A DynamoDB list (L): ordered Values.
using ValueList = std::vector<Value>;

/// A DynamoDB map (M): key/Value pairs kept in insertion (parse/response)
/// order. The Renderer sorts keys alphabetically at format time (arch §9); the
/// model keeps source order so the adapter stays trivial.
using ValueMap = std::vector<std::pair<std::string, Value>>;

/// The SDK-free attribute-value mirror. `type` selects the active field(s).
struct Value {
    enum class Type { S, N, B, BOOL, NUL, L, M, SS, NS, BS };

    Type type = Type::NUL;
    std::string s;                 ///< S, N (exact string), or B (base64)
    bool boolean = false;          ///< BOOL
    ValueList list;                ///< L
    ValueMap map;                  ///< M
    std::vector<std::string> set;  ///< SS / NS / BS

    static Value string_value(std::string v) {
        Value out;
        out.type = Type::S;
        out.s = std::move(v);
        return out;
    }

    static Value number_value(std::string exact) {
        Value out;
        out.type = Type::N;
        out.s = std::move(exact);
        return out;
    }

    static Value bool_value(bool b) {
        Value out;
        out.type = Type::BOOL;
        out.boolean = b;
        return out;
    }

    static Value null_value() {
        Value out;
        out.type = Type::NUL;
        return out;
    }
};

}  // namespace ddbsh

#endif  // DDBSH_VALUE_HPP
