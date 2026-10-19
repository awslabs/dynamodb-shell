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
/// `diagnostics` are byte-exact informational lines the CLI writes to STDERR
/// (the D-2e-6 on-demand-GSI-ignored message). They are DATA on a success
/// outcome — they do NOT flip the exit code — mirroring the TransactOutcome
/// stderr channel. Empty for every status except that one combined ALTER form.
struct StatusOutcome {
    std::string token;
    std::vector<std::string> diagnostics{};
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

/// The result of a bulk UPDATE / UPSERT / DELETE: the `(n read, m modified,
/// k ccf)` accounting line the original prints for every such command (arch §7,
/// update.cpp / delete.cpp). `read` counts items enumerated by the Query/Scan
/// (0 on the single-conditional-write path), `modified` counts successful
/// writes, and `ccf` counts conditional-check failures (a mismatch is DATA, not
/// an error — D-2c-7/-8). `consumed_capacity` is populated with ratelimit /
/// RETURN CONSUMED CAPACITY support (2g); nullopt for now. The Renderer emits
/// `"<VERB> (<read> read, <modified> modified, <ccf> ccf)\n"`.
struct BulkWriteOutcome {
    enum class Verb { update, upsert, del };
    Verb verb = Verb::update;
    long read = 0;
    long modified = 0;
    long ccf = 0;
    std::optional<ConsumedCapacity> consumed_capacity;
};

struct MultiOutcome;  // defined below (recursive variant member)

/// A transaction that did NOT return items: a terminal status (`ABORT`) plus
/// optional stderr diagnostics (D-2d-5/-6). Read-transaction SUCCESS renders
/// items via SelectOutcome and write-transaction SUCCESS renders `COMMIT` via
/// StatusOutcome — so this arm carries only the ABORT cases:
///   * pre-flight abort (ROLLBACK terminator, a non-reducible inner SELECT, or
///     an inner write lacking a full key): emitted as success data, exit 0; and
///   * service cancellation: emitted via the sink THEN the executor returns the
///     composed "Transaction failed. ..." Error (exit nonzero) — the ABORT
///     token on stdout precedes the CLI's stderr error render.
/// `diagnostics` are byte-exact messages the CLI writes to STDERR (the
/// non-reducible-SELECT message, D-2d-6); the `token` (`ABORT`) goes to STDOUT.
/// The golden harness captures both streams merged and -s sorts them, so
/// itxt-2's two lines match while a real terminal still separates them.
struct TransactOutcome {
    std::string token = "ABORT";           ///< currently always ABORT
    std::vector<std::string> diagnostics;  ///< stderr lines (may be empty)
};

/// The result of a successful read transaction (TransactGetItems): one row per
/// Get, in request order. Unlike SelectOutcome — which filters empty items to
/// match the original's `show_items` for a SELECT — a read transaction renders
/// EVERY response, including misses: the original's `CTransactReadCommand::run`
/// loop does `printf("%s\n", show_item(item))` unconditionally, and
/// `show_item` of an empty map returns "" (select_helper.cpp), so a missing
/// item prints as a blank line. Preserving the positional, one-line-per-Get
/// output (blank line for a miss) is the fidelity contract (tenet 1); a
/// read transaction does not totalize/display consumed capacity (the original's
/// transact-read run() never does), so there is no capacity field here.
struct TransactReadOutcome {
    std::vector<ItemRow> items;  ///< one per Get, request order; a miss -> blank line
};

/// Discriminated union over the observable shapes a command produces. Extended
/// per sub-phase. `MultiOutcome` carries several fragments emitted by ONE
/// statement (multi-row INSERT; future batched forms) in production order; it
/// is the buffering form of the streaming OutcomeSink (D-2c-12).
using ExecOutcome = std::variant<StatusOutcome, ReportOutcome, EmptyOutcome, SelectOutcome,
                                 WriteOutcome, ExplainOutcome, BulkWriteOutcome, TransactOutcome,
                                 TransactReadOutcome, MultiOutcome>;

/// Carries >1 fragment produced by one statement, in production order (e.g. a
/// multi-row INSERT emits one WriteOutcome per row). The buffering
/// Session::execute(stmt) wraps multiple sink-emitted fragments in a
/// MultiOutcome; the Renderer concatenates render(part) in order. The
/// `std::vector<ExecOutcome>` member is legal while ExecOutcome is incomplete
/// (the std::vector incomplete-type allowance — D-2c-14 assumption 1,
/// gcc/libstdc++ C++23).
struct MultiOutcome {
    std::vector<ExecOutcome> parts;
};

}  // namespace ddbsh

#endif  // DDBSH_EXEC_OUTCOME_HPP
