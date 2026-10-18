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
// (AlterTableCmd) and the SET TTL node (AlterTtlCmd, DescribeTimeToLive +
// UpdateTimeToLive). The PITR / REPLICA executors land in later sub-phases.
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

/// Execute an ALTER TABLE SET TTL ... (DescribeTimeToLive + UpdateTimeToLive;
/// the original CUpdateTableTTLCommand). Three sub-behaviors (D-2e-7):
///   1. DescribeTimeToLive is ALWAYS called first (even under EXPLAIN); a
///      failure returns the Error with no output (the original's -1 path).
///   2. Disabling an already-disabled table short-circuits to
///      StatusOutcome{"ALTER"} with NO UpdateTimeToLive call — and, matching
///      the original (which performs this check before its explaining() branch),
///      this fires even when `explain` is true.
///   3. Otherwise UpdateTimeToLive is issued: enable uses the user-supplied
///      attribute, disable reuses the existing name from the probe. On success
///      -> StatusOutcome{"ALTER"}; under EXPLAIN -> one `UpdateTimeToLive(...)`
///      block (the probe is live but NOT serialized), NO trailing status.
Result<ExecOutcome> execute_alter_ttl(const AlterTtlCmd& cmd, IDDBClient& client, bool explain);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_ALTER_TABLE_EXECUTOR_HPP
