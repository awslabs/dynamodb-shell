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

    /// Prepared-statement parameter marker (engine facility, D-P5-2). When > 0
    /// this Value is an UNRESOLVED positional parameter — the Nth `?` (1-based
    /// by appearance in the SQL text) — NOT a concrete value; it is replaced by
    /// a bound Value before the statement executes. 0 (the default) means an
    /// ordinary value. No new Type enum alternative is used precisely so every
    /// `switch (value.type)` in the serializer/renderer/adapter stays complete
    /// (a resolved statement carries no markers; an unresolved one is rejected
    /// before serialization). The CLI path never sets this (its SQL has no
    /// markers), so it is inert for every standard command.
    int param_ordinal = 0;

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

    /// A binary (B) value. `base64` is the standard-base64 encoding of the raw
    /// bytes (the same on-the-wire form the AWS adapter base64-decodes and that
    /// the item renderer / serializer carry). Mirrors how a B value read back
    /// from DynamoDB is stored (arch §9 / aws_ddb_client.cpp).
    static Value binary_value(std::string base64) {
        Value out;
        out.type = Type::B;
        out.s = std::move(base64);
        return out;
    }

    /// A string set (SS). Element order is not semantically significant —
    /// DynamoDB sets are unordered — so consumers that compare sets do so
    /// order-insensitively (the in-memory test client canonicalizes order).
    static Value string_set(std::vector<std::string> elements) {
        Value out;
        out.type = Type::SS;
        out.set = std::move(elements);
        return out;
    }

    /// A number set (NS). Each element is the exact decimal string of a number
    /// (DynamoDB arbitrary numeric precision, as for N). Unordered (see
    /// string_set).
    static Value number_set(std::vector<std::string> elements) {
        Value out;
        out.type = Type::NS;
        out.set = std::move(elements);
        return out;
    }

    /// A binary set (BS). Each element is the standard-base64 encoding of one
    /// binary member (see binary_value). Unordered (see string_set).
    static Value binary_set(std::vector<std::string> elements) {
        Value out;
        out.type = Type::BS;
        out.set = std::move(elements);
        return out;
    }

    /// An unresolved positional parameter marker (the `ordinal1`-th `?`,
    /// 1-based). Built by the grammar; replaced by a bound Value before
    /// execution (see param_ordinal).
    static Value param_marker(int ordinal1) {
        Value out;
        out.type = Type::NUL;
        out.param_ordinal = ordinal1;
        return out;
    }
};

}  // namespace ddbsh

#endif  // DDBSH_VALUE_HPP
