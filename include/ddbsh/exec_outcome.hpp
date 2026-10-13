// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// exec_outcome.hpp — the structured result of executing a Statement.
//
// Execution produces DATA (ExecOutcome), never pre-formatted bytes; the
// Renderer turns it into the exact golden bytes at the edge (P2a-03). The
// engine writes nothing to any stream.
//
// 2b scope: StatusOutcome, ReportOutcome, EmptyOutcome (walking skeleton) plus
// SelectOutcome (W5 — SELECT execution). BulkWriteOutcome,
// ConditionalWriteOutcome, transact/explain/help arms land in later sub-phases
// per the P2a-03 schema.

#ifndef DDBSH_EXEC_OUTCOME_HPP
#define DDBSH_EXEC_OUTCOME_HPP

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ddbsh/iddbclient.hpp"

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

/// The result of a SELECT: the returned item rows (in service order — the
/// Renderer sorts attribute names alphabetically, arch §9) plus the totalized
/// consumed capacity, present only when the user asked for it (RETURN CONSUMED
/// CAPACITY). The Renderer emits each non-empty item then the optional
/// `SELECT (table, cu, rcu, wcu)` line (arch §9). A ratelimit forces the
/// service to return capacity but does NOT populate this field (it is not
/// displayed), matching the original's display gating on the user's returns.
struct SelectOutcome {
    std::vector<ItemRow> items;
    std::optional<ConsumedCapacity> consumed_capacity;
};

/// Discriminated union over the observable shapes a command produces. Extended
/// per sub-phase (BulkWriteOutcome, ExplainOutcome, ...).
using ExecOutcome = std::variant<StatusOutcome, ReportOutcome, EmptyOutcome, SelectOutcome>;

}  // namespace ddbsh

#endif  // DDBSH_EXEC_OUTCOME_HPP
