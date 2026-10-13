// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// insert_executor.hpp — execute INSERT / REPLACE against the IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of CInsertCommand::run()
// and make_put_request() (arch §7). For each row it builds a PutItem request
// (column[i] -> value[i]); INSERT (insert=true) adds the conditional
// `attribute_not_exists(#pk)` guard so it fails on an existing primary key,
// while REPLACE (insert=false) is unconditional. One PutItem is issued per row
// and one status token (INSERT/REPLACE) is produced per row (arch §7).
//
// Fidelity (arch §6): the INSERT condition aliases the partition key through a
// fresh SymbolTable PER ROW, each seeded with the next per-statement alias
// prefix — exactly matching the original, which constructed a new CSymbolTable
// (consuming one unique prefix) inside make_put_request for every INSERT row.
// REPLACE builds no SymbolTable and consumes no prefix.
//
// EXPLAIN of an INSERT/REPLACE is NOT handled here (it renders the serialized
// PutItem request(s) instead — W6); the caller routes an explaining write
// elsewhere.
//
// D12: never aborts; a key-schema, validation, or service/conditional failure
// is returned as an Error.

#ifndef DDBSH_SRC_EXECUTE_INSERT_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_INSERT_EXECUTOR_HPP

#include <functional>
#include <string>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) INSERT or REPLACE. `next_prefix` mints the next
/// per-statement base-52 alias prefix (Session::next_alias_prefix); it is
/// called once per INSERT row for the condition's SymbolTable (never for
/// REPLACE), matching the original's per-row prefix consumption. Returns a
/// WriteOutcome carrying one status token per row written.
Result<ExecOutcome> execute_insert(const InsertCmd& cmd, IDDBClient& client,
                                   const std::function<std::string()>& next_prefix);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_INSERT_EXECUTOR_HPP
