// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// show_tables_executor.hpp — execute SHOW TABLES behind IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of the original
// command/show_tables.cpp: ListTables (paginated) followed by a DescribeTable +
// DescribeTimeToLive per listed table, filtered by the optional LIKE regex, and
// shaped into the pipe-delimited report rows (arch §7). The result is a
// ReportOutcome (the executor builds the lines; the Renderer joins them —
// P2a-03 §4.2). The engine never writes to a stream.
//
// D12: never aborts/throws across the boundary — an invalid LIKE regex is
// returned as an Error, not an uncaught std::regex_error (the original
// constructed std::regex(m_regexp) unguarded).

#ifndef DDBSH_SRC_EXECUTE_SHOW_TABLES_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_SHOW_TABLES_EXECUTOR_HPP

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) SHOW TABLES. Lists table names, applies the optional
/// LIKE regex, and for each match emits either just the name (BRIEF, or when a
/// per-table describe/ttl lookup fails) or the full status row.
Result<ExecOutcome> execute_show_tables(const ShowTablesCmd& cmd, IDDBClient& client);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_SHOW_TABLES_EXECUTOR_HPP
