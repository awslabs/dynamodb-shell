// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// exec_outcome.hpp — the structured result of executing a Statement.
//
// Execution produces DATA (ExecOutcome), never pre-formatted bytes; the
// Renderer turns it into the exact golden bytes at the edge (P2a-03). The
// engine writes nothing to any stream.
//
// 2a scope: only the payloads the walking skeleton needs (StatusOutcome,
// ReportOutcome, EmptyOutcome). SelectOutcome, BulkWriteOutcome,
// ConditionalWriteOutcome, transact/explain/help arms land in 2d/2e per the
// P2a-03 schema.

#ifndef DDBSH_EXEC_OUTCOME_HPP
#define DDBSH_EXEC_OUTCOME_HPP

#include <string>
#include <variant>
#include <vector>

namespace ddbsh {

/// A terse status token line: CONNECT/CREATE/DROP/ALTER/QUIT/... (arch §7).
struct StatusOutcome {
    std::string token;
};

/// A multi-line semi-structured human report: DESCRIBE, SHOW LIMITS,
/// SHOW CREATE TABLE, SHOW TABLES/BACKUPS, DESCRIBE BACKUP. The command builds
/// the lines (report shaping, not I/O); the Renderer joins them (P2a-03 §4.2).
struct ReportOutcome {
    std::vector<std::string> lines;
};

/// Blank-line / comment-only input: renders nothing.
struct EmptyOutcome {};

/// Discriminated union over the observable shapes a command produces. Extended
/// per sub-phase (SelectOutcome, BulkWriteOutcome, ExplainOutcome, ...).
using ExecOutcome = std::variant<StatusOutcome, ReportOutcome, EmptyOutcome>;

}  // namespace ddbsh

#endif  // DDBSH_EXEC_OUTCOME_HPP
