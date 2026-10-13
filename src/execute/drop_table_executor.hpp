// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// drop_table_executor.hpp — execute DROP TABLE against the IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of
// CDropTableCommand::run() (arch §7): issues DeleteTable and, unless NOWAIT,
// waits for the table to be gone. IF EXISTS first probes existence via
// describe_table and silently succeeds (no output) when the table is absent,
// exactly as the original (which prints nothing and returns 0 in that case).
//
// EXPLAIN of a DROP is NOT handled here (it must not contact the service and
// renders the serialized DeleteTable request instead — W6); the caller routes
// an explaining DROP elsewhere.
//
// D12: never aborts; a service or waiter failure is returned as an Error.

#ifndef DDBSH_SRC_EXECUTE_DROP_TABLE_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_DROP_TABLE_EXECUTOR_HPP

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) DROP TABLE. Returns a StatusOutcome{"DROP"} on a
/// successful delete, or an EmptyOutcome when IF EXISTS matched a missing
/// table (no output, matching the original).
Result<ExecOutcome> execute_drop_table(const DropTableCmd& cmd, IDDBClient& client);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_DROP_TABLE_EXECUTOR_HPP
