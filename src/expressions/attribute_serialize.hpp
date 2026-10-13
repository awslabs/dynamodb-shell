// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// attribute_serialize.hpp — literal serialization of a Value into the DynamoDB
// wire-JSON shape, for the symbol-table-less serialize path.
//
// PRIVATE engine header (not installed). The original expression nodes, when
// handed a null CSymbolTable*, rendered their right-hand values via
// serialize_attribute() = AttributeValue::SerializeAttribute() with newlines
// stripped (dynamodb/serialize_attribute.cpp). In the reimplementation the
// in-scope SELECT paths (GetItem/Query/Scan) ALWAYS serialize through a symbol
// table (the values become ExpressionAttributeValues and the string carries
// `:v<prefix><n>` tokens), so this literal path is not exercised by any 2b
// golden. It is provided for completeness and faithfulness of the ported base
// class (whose serialize() accepts a null symbol table).
//
// TODO(fidelity): when a command family that uses the null-symbol-table
// serialize path lands and becomes golden-relevant, verify these bytes against
// the AWS SDK's AttributeValue::SerializeAttribute() output and pin with a
// golden. Until then this is SDK-shape-approximate, not byte-verified.

#ifndef DDBSH_SRC_EXPRESSIONS_ATTRIBUTE_SERIALIZE_HPP
#define DDBSH_SRC_EXPRESSIONS_ATTRIBUTE_SERIALIZE_HPP

#include <string>

#include "ddbsh/value.hpp"

namespace ddbsh::engine {

/// Render a Value as DynamoDB wire JSON (e.g. {"S":"foo"}, {"N":"5"}),
/// newlines stripped, matching the original serialize_attribute() shape.
std::string serialize_attribute(const Value& value);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_ATTRIBUTE_SERIALIZE_HPP
