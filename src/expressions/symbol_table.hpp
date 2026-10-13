// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// symbol_table.hpp — deterministic ExpressionAttributeNames / Values builder.
//
// PRIVATE engine header (not installed). SDK-free port of the original
// expressions/symbol_table.hpp (CSymbolTable). Generates `#a<prefix><n>`
// attribute-name aliases and `:v<prefix><n>` value aliases, 1-based, where
// `<prefix>` is the per-statement base-52 prefix (see prefix.hpp). The exact
// alias scheme is a fidelity contract (arch §6, §14 item 1) — EXPLAIN output
// is byte-compared against the original.
//
// Fidelity rules preserved from the original:
//   * Attribute names DEDUPE by real name (a repeated attribute reuses its
//     token); values NEVER dedupe (each new_value() mints a fresh token).
//   * Dotted paths are tokenized per segment: `a.b` -> `#a<p>1.#a<p>2`.
//   * `[subscript]` suffixes are preserved literally on the segment token:
//     `a[3]` -> `#a<p>1[3]`.
//   * get_names() returns the INVERTED map (token -> real name) for use as
//     ExpressionAttributeNames; get_values() returns token -> Value for
//     ExpressionAttributeValues. Both are std::map (ordered by token), so the
//     Renderer emits them in the same sorted order the SDK's std::map did.
//
// D11: SDK-free — values are carried as ddbsh::Value, not Aws:: types.

#ifndef DDBSH_SRC_EXPRESSIONS_SYMBOL_TABLE_HPP
#define DDBSH_SRC_EXPRESSIONS_SYMBOL_TABLE_HPP

#include <map>
#include <string>
#include <utility>

#include "ddbsh/value.hpp"

namespace ddbsh::engine {

class SymbolTable {
public:
    /// @param prefix the per-statement base-52 prefix (from unique_prefix()).
    explicit SymbolTable(std::string prefix) : prefix_(std::move(prefix)) {}

    /// Add an attribute path, returning its token form. A dotted path is split
    /// on '.' and each segment aliased independently (names dedupe); a
    /// `[subscript]` suffix on a segment is retained verbatim on the token.
    std::string new_attribute(const std::string& name);

    /// Add a value, returning a freshly minted `:v<prefix><n>` token. Values
    /// never dedupe.
    std::string new_value(Value value);

    bool has_names() const { return !names_.empty(); }
    bool has_values() const { return !values_.empty(); }

    /// ExpressionAttributeNames: token -> real attribute name (inverted from
    /// the internal real-name-keyed map).
    std::map<std::string, std::string> get_names() const;

    /// ExpressionAttributeValues: token -> Value.
    const std::map<std::string, Value>& get_values() const { return values_; }

private:
    // One segment of a dotted path; splits off a trailing `[subscript]` and
    // aliases only the bare name part, re-appending the subscript to the token.
    std::string new_segment(const std::string& name);
    // A bare attribute segment (no '.' and no '['); dedupes by real name.
    std::string new_bare(const std::string& name);

    std::string prefix_;
    int attr_ix_ = 1;
    int value_ix_ = 1;
    std::map<std::string, std::string> names_;  // real name -> token
    std::map<std::string, Value> values_;       // token -> Value
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_SYMBOL_TABLE_HPP
