// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// param_bind.hpp — prepared-statement parameter-marker collection (D-P5-2).
//
// PRIVATE engine header (not installed). The grammar turns each `?` into a
// Value marker (Value::param_ordinal > 0) wherever an attribute value is
// expected (lee_rhs / insert_value). These helpers walk a parsed command and
// locate those markers so the embedding prepared-statement facility can resolve
// them in place with bound Values before execution (Approach H, D3): parse
// once, bind, execute, re-execute — without re-parsing.
//
// The returned pointers alias into the owning ParsedStatement's AST and stay
// valid for its lifetime (the AST is not reallocated across executions), so a
// prepared statement can be re-bound and re-executed repeatedly.

#ifndef DDBSH_SRC_PARSER_PARAM_BIND_HPP
#define DDBSH_SRC_PARSER_PARAM_BIND_HPP

#include <cstddef>
#include <vector>

#include "ddbsh/value.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Collect pointers to every unresolved `?` marker slot in `command`, across
/// the DML families that can carry a value (INSERT rows, WHERE trees, UPDATE
/// SET operands, the vector-search list, and a transaction's inner items),
/// recursing into nested L/M containers. Order is traversal order; callers key
/// off each slot's Value::param_ordinal (1-based by text appearance), not the
/// collection order.
void collect_param_slots(CommandNode& command, std::vector<Value*>& out);

/// Count the unresolved parameter markers still present in a parsed statement
/// (a const probe used by the executor to reject an unbound `?` that reached
/// execution outside the prepared bind path). A resolved prepared statement
/// reports 0 (its markers were overwritten with bound Values).
std::size_t count_param_markers(const ParsedStatement& statement);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PARSER_PARAM_BIND_HPP
