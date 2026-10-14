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
// SelectOutcome (W5 — SELECT execution), WriteOutcome (W5 part 2 —
// INSERT/REPLACE), and ExplainOutcome (W6 — EXPLAIN of the in-scope commands).
// BulkWriteOutcome, ConditionalWriteOutcome, transact/help arms land in later
// sub-phases per the P2a-03 schema.

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

/// The result of an INSERT / REPLACE: one status token per row written
/// (arch §7 — a multi-row `VALUES (..),(..)` emits one `INSERT`/`REPLACE` line
/// per row, in order). One `CInsertCommand` backs both verbs in the original,
/// so `tokens` holds the already-chosen verb string ("INSERT" or "REPLACE")
/// repeated per row; the Renderer emits each on its own line. (P2a-03 §4.1
/// flagged `MultiOutcome` vs a per-row vector as a developer choice; a single
/// outcome carrying the row tokens is simpler, byte-identical, and avoids a
/// self-referential variant — chosen here. RETURN VALUES payloads land with
/// return-values support in a later sub-phase.)
struct WriteOutcome {
    std::vector<std::string> tokens;
};

/// One serialized DynamoDB API request in an EXPLAIN block (P2a-03 §4.3).
/// `api_name` is the operation the original wrapped the request in — "GetItem",
/// "Query", "Scan", "PutItem", "CreateTable", "DeleteTable", "DescribeTable",
/// "DescribeTimeToLive", "DescribeContinuousBackups",
/// "DescribeTableReplicaAutoScaling". `body` is the SDK's RAW SerializePayload()
/// output (tab-indented JSON) — the serialization contract, produced at the
/// adapter where the request is built (D11); the engine never inspects it.
/// `format` selects the Renderer transform the original applied for that call:
///   * `pretty`  — explain_string (tab -> 3 spaces): the multi-line
///     `ApiName({\n   "k":   "v"\n})` block used for most requests (arch §9).
///   * `compact` — strip_newlines (newlines+tabs -> a single line, `:\t` ->
///     `: `): the one-line form the original emitted for DescribeTable and
///     DescribeTimeToLive via CDescribeHelper (describe_helper.cpp).
/// Keeping the body raw lets the Renderer own the transform (unit-testable with
/// a hand-built block, no SDK), per P2a-03 §5.
struct ExplainBlock {
    enum class Format { pretty, compact };
    std::string api_name;
    std::string body;
    Format format = Format::pretty;
};

/// EXPLAIN <command>: the serialized API request(s) the command WOULD issue,
/// rendered instead of executed (arch §7). Blocks are emitted in call order,
/// each as `ApiName(<formatted body>)\n`. `trailing_status` carries a status
/// token the original prints even in EXPLAIN mode: CREATE TABLE's run() prints
/// "CREATE" OUTSIDE the explain branch (create_table.cpp), so EXPLAIN CREATE
/// emits the CreateTable block AND the CREATE token; the other in-scope
/// commands leave it nullopt (EXPLAIN DROP prints only DeleteTable — its DROP
/// token lives in the non-explain branch, drop_table.cpp).
struct ExplainOutcome {
    std::vector<ExplainBlock> blocks;
    std::optional<std::string> trailing_status;
};

/// Discriminated union over the observable shapes a command produces. Extended
/// per sub-phase (BulkWriteOutcome, ...).
using ExecOutcome = std::variant<StatusOutcome, ReportOutcome, EmptyOutcome, SelectOutcome,
                                 WriteOutcome, ExplainOutcome>;

}  // namespace ddbsh

#endif  // DDBSH_EXEC_OUTCOME_HPP
