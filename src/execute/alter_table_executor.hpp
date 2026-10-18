// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// alter_table_executor.hpp — execute ALTER TABLE against the IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of the original
// CUpdateTableCommand::run() for the SET (non-GSI) forms (arch §7). It copies
// the parsed ALTER definition into an UpdateTableIn request and issues
// update_table; on success it emits the `ALTER` status token. Unlike CREATE
// (which prints its token even under EXPLAIN), the original ALTER prints the
// serialized request inside the explaining() branch and returns BEFORE the
// "ALTER" printf, so EXPLAIN carries NO trailing status (D-2e-11).
//
// The four oracle ALTER command classes split into four AST nodes sharing one
// StatementKind::alter_table (D-2e-1); this file handles the UpdateTable node
// (AlterTableCmd). The TTL / PITR / REPLICA executors land in later sub-phases.
//
// D12: never aborts; a service failure is returned as an Error (the CLI adapter
// composes the byte-exact "Update Table failed. ..." message, D-2e-13).

#ifndef DDBSH_SRC_EXECUTE_ALTER_TABLE_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_ALTER_TABLE_EXECUTOR_HPP

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) ALTER TABLE SET ... Returns StatusOutcome{"ALTER"}
/// on a successful UpdateTable, or an Error on failure.
Result<ExecOutcome> execute_alter_table(const AlterTableCmd& cmd, IDDBClient& client);

/// EXPLAIN an ALTER TABLE SET ...: serialize the UpdateTable request into an
/// ExplainOutcome (one `UpdateTable(...)` block, NO trailing status — D-2e-11).
Result<ExecOutcome> execute_alter_table_explain(const AlterTableCmd& cmd, IDDBClient& client);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_ALTER_TABLE_EXECUTOR_HPP
