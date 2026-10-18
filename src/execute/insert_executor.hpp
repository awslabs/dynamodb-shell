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
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) INSERT or REPLACE, emitting one WriteOutcome per
/// written row to `sink` BEFORE a failing row's Error is returned (AI-4 /
/// D-2c-12). This reproduces the original, which printed each row's status
/// token as it wrote and the error after the already-printed tokens
/// (insert.cpp). Returns success when all rows wrote, or the Error of the first
/// failing row (the successful rows are already emitted to the sink).
///
/// `next_prefix` mints the next per-statement base-52 alias prefix
/// (Session::next_alias_prefix); it is called once per INSERT row for the
/// condition's SymbolTable (never for REPLACE), matching the original's per-row
/// prefix consumption.
Result<void> execute_insert_streaming(const InsertCmd& cmd, IDDBClient& client,
                                      const std::function<std::string()>& next_prefix,
                                      OutcomeSink& sink);

/// EXPLAIN an INSERT/REPLACE: build the same per-row PutItem request(s) and
/// serialize each into an ExplainOutcome (one PutItem block per row), without
/// issuing them. Still resolves the key schema (for the INSERT condition) and
/// consumes the per-row alias prefix exactly as execute_insert, so the EXPLAIN
/// alias numbering matches a real run. Returns the ExplainOutcome or an Error.
Result<ExecOutcome> execute_insert_explain(const InsertCmd& cmd, IDDBClient& client,
                                           const std::function<std::string()>& next_prefix);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_INSERT_EXECUTOR_HPP
