// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// session.cpp — Session implementation (PIMPL).
//
// Impl holds the per-session state (config, the borrowed IDDBClient, and — in
// later sub-phases — the alias-prefix counter and schema cache). The scanner/
// parser handles live only inside parse() via a Driver, never on Impl (P2a-02
// §7). The Session destructor is defined HERE, where Impl is complete, per the
// PIMPL rule (a header-defaulted dtor would fail against the incomplete Impl).

#include "ddbsh/session.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "ddbsh/version.hpp"
#include "session_internal.hpp"

#include "execute/alter_table_executor.hpp"
#include "execute/backup_restore_executor.hpp"
#include "execute/create_table_executor.hpp"
#include "execute/delete_executor.hpp"
#include "execute/describe_executor.hpp"
#include "execute/drop_table_executor.hpp"
#include "execute/insert_executor.hpp"
#include "execute/select_executor.hpp"
#include "execute/show_commands_executor.hpp"
#include "execute/show_tables_executor.hpp"
#include "execute/transact_executor.hpp"
#include "execute/update_executor.hpp"
#include "expressions/prefix.hpp"
#include "parser/driver.hpp"
#include "parser/help_text.hpp"
#include "parser/param_bind.hpp"

namespace ddbsh {

namespace {

/// Fold the fragments of a command that streams per response (P12) back into
/// the one outcome the buffering Session::execute(stmt) returned before P12
/// (D-P12-15). Returns nullopt for every other command, whose fragments keep
/// the generic 0 / 1 / MultiOutcome shape.
///   * SHOW TABLES / SHOW BACKUPS / SHOW LIMITS / DESCRIBE: every
///     ReportOutcome's lines, in order, in one ReportOutcome (empty when the
///     command emitted nothing, e.g. SHOW TABLES over no tables).
///   * bulk UPDATE / UPSERT / DELETE under EXPLAIN: every ExplainOutcome's
///     blocks, in order, in one ExplainOutcome.
///   * bulk UPDATE / UPSERT / DELETE with RETURN VALUES: the partial
///     fragments' items, in order, ahead of the final fragment's, in that one
///     final BulkWriteOutcome.
std::optional<ExecOutcome> fold_streamed(const Statement& statement,
                                         std::vector<ExecOutcome>& parts) {
    const bool explain = statement.ast() != nullptr && statement.ast()->explain;
    switch (statement.kind()) {
        case StatementKind::show_tables:
        case StatementKind::show_backups:
        case StatementKind::show_limits:
        case StatementKind::describe: {
            if (explain) {
                return std::nullopt;  // EXPLAIN DESCRIBE emits one ExplainOutcome
            }
            ReportOutcome report;
            for (auto& part : parts) {
                if (auto* r = std::get_if<ReportOutcome>(&part)) {
                    std::move(r->lines.begin(), r->lines.end(), std::back_inserter(report.lines));
                }
            }
            return ExecOutcome{std::move(report)};
        }
        case StatementKind::update_statement:
        case StatementKind::del: {
            if (parts.size() < 2) {
                return std::nullopt;
            }
            // Every fragment is of one arm (the executors emit only
            // ExplainOutcome under EXPLAIN, only BulkWriteOutcome otherwise);
            // anything else falls back to the generic shape.
            if (std::holds_alternative<ExplainOutcome>(parts.front())) {
                ExplainOutcome merged;
                for (auto& part : parts) {
                    auto* e = std::get_if<ExplainOutcome>(&part);
                    if (e == nullptr) {
                        return std::nullopt;
                    }
                    std::move(e->blocks.begin(), e->blocks.end(),
                              std::back_inserter(merged.blocks));
                    if (e->trailing_status.has_value()) {
                        merged.trailing_status = std::move(e->trailing_status);
                    }
                }
                return ExecOutcome{std::move(merged)};
            }
            std::vector<ItemRow> items;
            for (auto& part : parts) {
                auto* b = std::get_if<BulkWriteOutcome>(&part);
                if (b == nullptr) {
                    return std::nullopt;
                }
                std::move(b->returned_items.begin(), b->returned_items.end(),
                          std::back_inserter(items));
            }
            BulkWriteOutcome final_part = std::move(std::get<BulkWriteOutcome>(parts.back()));
            final_part.returned_items = std::move(items);
            final_part.partial = false;
            return ExecOutcome{std::move(final_part)};
        }
        default:
            return std::nullopt;
    }
}

}  // namespace

struct Session::Impl {
    SessionConfig config;
    IDDBClient& client;  // borrowed; the embedder guarantees it outlives us

    // Per-session statement counter backing the deterministic expression-alias
    // prefix (arch §14 item 1). The original kept this in the CDDBSh singleton
    // (a process global, T1); the Phase-2b rules move it onto the Session so
    // concurrent sessions never share alias state. next_alias_prefix() mints
    // the next prefix (0 -> "aaa", 1 -> "baa", ...); the planner consumes it
    // when it builds a SymbolTable (wired in W4, when SELECT executes).
    unsigned long alias_prefix_counter = 0;

    std::string next_alias_prefix() { return engine::unique_prefix(alias_prefix_counter++); }

    // The streaming overload's body: runs the statement against the sink.
    // Session::execute(stmt, sink) wraps it to call sink.end_statement()
    // afterwards (D-P12-16). Kept here, not on Session, so the public header
    // carries no implementation detail.
    Result<void> dispatch(const Statement& statement, OutcomeSink& sink);

    Impl(SessionConfig cfg, IDDBClient& c) : config(std::move(cfg)), client(c) {}
};

Session::Session(SessionConfig cfg, IDDBClient& client)
    : impl_(std::make_unique<Impl>(std::move(cfg), client)) {}

Session::~Session() = default;
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;

Result<Statement> Session::parse(std::string_view text) {
    engine::Driver driver(text);
    if (!driver.parse()) {
        if (driver.error().has_value()) {
            return fail(*driver.error());
        }
        return fail(Errc::parse_error, "parse error");
    }
    // On success the grammar always sets a result.
    return driver.release_result();
}

ParsedBatch Session::parse_batch(std::string_view text) {
    ParsedBatch batch;
    engine::Driver driver(text);
    for (;;) {
        engine::Driver::Step step = driver.parse_next();
        if (step == engine::Driver::Step::error) {
            // The statement after the ones already collected failed to parse.
            // Keep the good prefix ("execute what you can") and surface the
            // error for the embedder to render after it.
            batch.error = driver.error().has_value()
                              ? *driver.error()
                              : Error{Errc::parse_error, "parse error", std::nullopt, std::nullopt};
            break;
        }
        if (step == engine::Driver::Step::empty_at_eof) {
            break;  // end of the batch
        }
        batch.statements.push_back(driver.release_result());
    }
    return batch;
}

Result<ExecOutcome> Session::execute(const Statement& statement) {
    // Buffering convenience: collect every emitted fragment, then fold them
    // into a single ExecOutcome (0 -> the command's natural single outcome,
    // 1 -> that fragment, >1 -> MultiOutcome). On failure the Error is returned
    // and buffered fragments are discarded (D-2c-12). The streaming overload is
    // the primary; this wraps it with a collecting sink.
    // An unbound `?` parameter marker must never reach serialization (it would
    // become a bogus NULL request). A statement carrying markers is only legal
    // through the prepared-statement bind path (which resolves them in place
    // before execute); reaching execute with markers still present is caller
    // misuse (D-P5-2). No standard CLI command ever carries a marker, so this
    // guard never fires on the golden path.
    if (statement.ast() != nullptr && engine::count_param_markers(*statement.ast()) > 0) {
        return fail(Errc::unbound_parameter,
                    "statement has unbound parameter marker(s); use prepare() and bind()");
    }

    struct BufferingSink : OutcomeSink {
        std::vector<ExecOutcome> parts;
        void emit(const ExecOutcome& fragment) override { parts.push_back(fragment); }
    } sink;

    // A non-EXPLAIN SELECT buffers into ONE SelectOutcome / VectorSearchOutcome
    // (the buffering contract the native tests and the embedding connection
    // layer rely on). The streaming overload emits per-page fragments for
    // bounded CLI memory (D-P5-1); routing the buffering overload through it
    // would fold a multi-page SELECT into a MultiOutcome of SelectOutcomes.
    // Short-circuit to the buffering executor so this overload's result shape
    // is unchanged.
    if (statement.kind() == StatementKind::select_statement && !statement.ast()->explain) {
        const auto& select = std::get<engine::SelectCmd>(statement.ast()->command);
        return engine::execute_select(select, impl_->client, impl_->next_alias_prefix());
    }

    auto streamed = execute(statement, sink);
    if (!streamed.has_value()) {
        return fail(streamed.error());
    }
    // The commands that stream per response since P12 return the single
    // outcome they returned before it (D-P12-15).
    if (auto folded = fold_streamed(statement, sink.parts); folded.has_value()) {
        return std::move(*folded);
    }
    if (sink.parts.empty()) {
        return ExecOutcome{EmptyOutcome{}};
    }
    if (sink.parts.size() == 1) {
        return std::move(sink.parts.front());
    }
    return ExecOutcome{MultiOutcome{std::move(sink.parts)}};
}

Result<void> Session::execute(const Statement& statement, OutcomeSink& sink) {
    // Every call ends with exactly one end_statement(), after the last emit,
    // on success and on failure (P12, D-P12-16), so a sink holding an open
    // result set closes it before the caller reports any error.
    Result<void> result = impl_->dispatch(statement, sink);
    sink.end_statement();
    return result;
}

Result<void> Session::Impl::dispatch(const Statement& statement, OutcomeSink& sink) {
    // Reject an unbound `?` marker before any serialization (see the buffering
    // overload); markers are legal only through the prepared bind path.
    if (statement.ast() != nullptr && engine::count_param_markers(*statement.ast()) > 0) {
        return fail(Errc::unbound_parameter,
                    "statement has unbound parameter marker(s); use prepare() and bind()");
    }

    // Emit a single buffered outcome (the common case for every command that
    // does not stream per-row), or propagate its Error unemitted.
    auto emit_single = [&sink](Result<ExecOutcome> outcome) -> Result<void> {
        if (!outcome.has_value()) {
            return fail(outcome.error());
        }
        sink.emit(*outcome);
        return {};
    };

    switch (statement.kind()) {
        case StatementKind::empty:
            return emit_single(ExecOutcome{EmptyOutcome{}});
        case StatementKind::show_version:
            // Faithful to the original: SHOW VERSION prints "ddbsh - version
            // <v>\n". In the original this happens inside the parser ACTION for
            // show_version_command (parser.y:646-648: builds no command, prints
            // the line during the parse), NOT via the startup banner
            // (main.cpp:359, which is a separate non-quiet-only path). We move
            // that side effect out of the parser into execute() and carry the
            // line as data (ReportOutcome) so the engine never writes to a
            // stream (P2a-03); the Renderer emits the identical bytes.
            return emit_single(ExecOutcome{ReportOutcome{{"ddbsh - version " + version()}}});
        case StatementKind::show_connection: {
            // SHOW CONNECTION reports the running composition (phase 6f,
            // D-P6f-9): the library version, the session's region/endpoint, and
            // the injected client's transport/backend binding. Additive and
            // terminal-free — the same carry-the-line-as-data pattern SHOW
            // VERSION uses, so the engine writes to no stream. Contacts no API.
            const std::string& region = config.region;
            const std::string& endpoint = config.endpoint;
            std::vector<std::string> lines;
            lines.push_back("ddbsh - version " + version());
            lines.push_back("Region: " + (region.empty() ? std::string("(default)") : region));
            lines.push_back("Endpoint: " +
                            (endpoint.empty() ? std::string("(default)") : endpoint));
            lines.push_back("Backend: " + client.connection_descriptor());
            return emit_single(ExecOutcome{ReportOutcome{std::move(lines)}});
        }
        case StatementKind::quit:
            return emit_single(ExecOutcome{StatusOutcome{"QUIT"}});
        case StatementKind::help: {
            // HELP contacts no API (arch §7). The original printed the usage
            // text during the parse (parser.y help_command); we carry the topic
            // on the AST and emit the identical bytes here as a ReportOutcome,
            // keeping the engine terminal-free (same pattern as SHOW VERSION).
            const auto& help = std::get<engine::HelpCmd>(statement.ast()->command);
            return emit_single(
                ExecOutcome{ReportOutcome{engine::help_lines(engine::help_text(help.topic))}});
        }
        case StatementKind::connect: {
            // CONNECT updates the session's region/endpoint AND reconnects the
            // injected client so every later statement (in this batch and
            // across batches) targets the new region/endpoint — faithfully
            // matching the original CConnectCommand, which calls
            // setRegionAndEndpoint + reconnect (arch §3/§7; B11 / D-2j-10).
            // The original's reconnect() was non-fatal (it logged and
            // continued, still emitting CONNECT), so a connect() failure is
            // deliberately NOT surfaced as this statement's Error (open
            // question #2, default: match the original). The CLI's AwsDDBClient
            // re-resolves credentials and rebuilds its SDK client; the test
            // doubles inherit the no-op default.
            const auto& connect = std::get<engine::ConnectCmd>(statement.ast()->command);
            config.region = connect.region;
            config.endpoint = connect.endpoint;
            (void)client.connect(connect.region, connect.endpoint);
            return emit_single(ExecOutcome{StatusOutcome{"CONNECT"}});
        }
        case StatementKind::select_statement: {
            const auto& select = std::get<engine::SelectCmd>(statement.ast()->command);
            if (statement.ast()->explain) {
                // EXPLAIN renders the serialized GetItem/Query/Scan request
                // (the planner still resolves the key schema; it does not read
                // data). W6.
                return emit_single(
                    engine::execute_select_explain(select, client, next_alias_prefix()));
            }
            // Stream the SELECT with bounded memory: one SelectOutcome fragment
            // per page, then a final capacity fragment if the user asked
            // (D-P5-1 / D2). Byte-identical to the old single-outcome path. The
            // CLI's RenderingSink renders each page and drops it, so a large
            // Scan is no longer resident in memory.
            return engine::execute_select_streaming(select, client, next_alias_prefix(), sink);
        }
        case StatementKind::drop_table: {
            const auto& drop = std::get<engine::DropTableCmd>(statement.ast()->command);
            if (statement.ast()->explain) {
                // EXPLAIN renders the serialized DeleteTable request (IF EXISTS
                // still probes existence; a missing table emits nothing). W6.
                return emit_single(engine::execute_drop_table_explain(drop, client));
            }
            return emit_single(engine::execute_drop_table(drop, client));
        }
        case StatementKind::insert: {
            const auto& insert = std::get<engine::InsertCmd>(statement.ast()->command);
            if (statement.ast()->explain) {
                // EXPLAIN renders the serialized PutItem request(s), one per
                // row (the key schema is still resolved for the condition). W6.
                return emit_single(engine::execute_insert_explain(
                    insert, client, [this] { return next_alias_prefix(); }));
            }
            // Streaming: emit one WriteOutcome per written row BEFORE a failing
            // row's Error, so partial output survives the failure (AI-4 /
            // D-2c-12) — the original printed each row's token then the error.
            return engine::execute_insert_streaming(
                insert, client, [this] { return next_alias_prefix(); }, sink);
        }
        case StatementKind::update_statement: {
            const auto& update = std::get<engine::UpdateCmd>(statement.ast()->command);
            return engine::execute_update(
                update, client, [this] { return next_alias_prefix(); }, statement.ast()->explain,
                sink);
        }
        case StatementKind::del: {
            const auto& del = std::get<engine::DeleteCmd>(statement.ast()->command);
            return engine::execute_delete(
                del, client, [this] { return next_alias_prefix(); }, statement.ast()->explain,
                sink);
        }
        case StatementKind::transaction: {
            const auto& txn = std::get<engine::TransactCmd>(statement.ast()->command);
            return engine::execute_transact(
                txn, client, [this] { return next_alias_prefix(); }, statement.ast()->explain,
                sink);
        }
        case StatementKind::create_table: {
            const auto& create = std::get<engine::CreateTableCmd>(statement.ast()->command);
            if (statement.ast()->explain) {
                // EXPLAIN renders the serialized CreateTable request plus the
                // CREATE token the original prints even when explaining. W6.
                return emit_single(engine::execute_create_table_explain(create, client));
            }
            return emit_single(engine::execute_create_table(create, client));
        }
        case StatementKind::alter_table: {
            // The four ALTER AST nodes share one StatementKind::alter_table
            // (D-2e-1); dispatch on the active node. UpdateTable (AlterTableCmd),
            // SET TTL (AlterTtlCmd), SET PITR (AlterPitrCmd), and REPLICA
            // (AlterReplicaCmd) — all four land here.
            const auto& command = statement.ast()->command;
            const bool explain = statement.ast()->explain;
            if (std::holds_alternative<engine::AlterTableCmd>(command)) {
                const auto& alter = std::get<engine::AlterTableCmd>(command);
                if (explain) {
                    // EXPLAIN renders the serialized UpdateTable request with NO
                    // trailing status token (contrast CREATE) — D-2e-11.
                    return emit_single(engine::execute_alter_table_explain(alter, client));
                }
                return emit_single(engine::execute_alter_table(alter, client));
            }
            if (std::holds_alternative<engine::AlterTtlCmd>(command)) {
                // SET TTL: DescribeTimeToLive + UpdateTimeToLive (the executor
                // owns the probe-first / already-disabled short-circuit / EXPLAIN
                // logic, D-2e-7), so pass the explain flag through.
                const auto& ttl = std::get<engine::AlterTtlCmd>(command);
                return emit_single(engine::execute_alter_ttl(ttl, client, explain));
            }
            if (std::holds_alternative<engine::AlterPitrCmd>(command)) {
                // SET PITR: UpdateContinuousBackups (the executor owns the EXPLAIN
                // `UpdateContinuousBackupsRequest(...)` label quirk, D-2e-8/-11).
                const auto& pitr = std::get<engine::AlterPitrCmd>(command);
                return emit_single(engine::execute_alter_pitr(pitr, client, explain));
            }
            // REPLICA: UpdateTable with one ReplicationGroupUpdate (D-2e-9); the
            // executor owns the EXPLAIN `UpdateTable(...)` block.
            const auto& replica = std::get<engine::AlterReplicaCmd>(command);
            return emit_single(engine::execute_alter_replica(replica, client, explain));
        }
        case StatementKind::show_tables: {
            // SHOW TABLES is not explainable (it is a show_command, not in the
            // grammar's `explainable` set — SHOW/CONNECT/QUIT never carry the
            // EXPLAIN flag), so there is no explain path to guard here.
            // Streams one row per fragment as its describes return (P12).
            const auto& show = std::get<engine::ShowTablesCmd>(statement.ast()->command);
            return engine::execute_show_tables(show, client, sink);
        }
        case StatementKind::describe:
            if (statement.ast()->explain) {
                // EXPLAIN renders the four serialized DescribeTable/
                // DescribeTimeToLive/DescribeContinuousBackups/
                // DescribeTableReplicaAutoScaling requests; no service call. W6.
                return emit_single(engine::execute_describe_explain(
                    std::get<engine::DescribeCmd>(statement.ast()->command), client));
            }
            // Streams up to three fragments, one per response group (P12).
            return engine::execute_describe(std::get<engine::DescribeCmd>(statement.ast()->command),
                                            client, sink);
        case StatementKind::create_backup:
            // BACKUP TABLE ... CALLED ... — a ddl command, so explainable.
            return emit_single(engine::execute_create_backup(
                std::get<engine::CreateBackupCmd>(statement.ast()->command), client,
                statement.ast()->explain));
        case StatementKind::drop_backup:
            return emit_single(engine::execute_drop_backup(
                std::get<engine::DropBackupCmd>(statement.ast()->command), client,
                statement.ast()->explain));
        case StatementKind::describe_backup:
            return emit_single(engine::execute_describe_backup(
                std::get<engine::DescribeBackupCmd>(statement.ast()->command), client,
                statement.ast()->explain));
        case StatementKind::restore:
            return emit_single(
                engine::execute_restore(std::get<engine::RestoreCmd>(statement.ast()->command),
                                        client, statement.ast()->explain));
        case StatementKind::show_backups:
            // SHOW commands are not explainable (grammar: show_command is not in
            // the explainable set, matching the oracle — EXPLAIN SHOW ... is a
            // syntax error), so there is no explain path here.
            // Streams one fragment per ListBackups page (P12).
            return engine::execute_show_backups(
                std::get<engine::ShowBackupsCmd>(statement.ast()->command), client, sink);
        case StatementKind::show_limits:
            // The `Region:` line is sourced from the session's current region
            // (D-2f-9), threaded as a parameter — not from the client seam.
            // Streams the header after DescribeLimits, then the totals (P12).
            return engine::execute_show_limits(
                std::get<engine::ShowLimitsCmd>(statement.ast()->command), client, config.region,
                sink);
        case StatementKind::show_create_table:
            return emit_single(engine::execute_show_create_table(
                std::get<engine::ShowCreateTableCmd>(statement.ast()->command), client));
    }
    return fail(Errc::internal_error, "unhandled statement kind");
}

Result<ExecOutcome> Session::run(std::string_view text) {
    auto stmt = parse(text);
    if (!stmt.has_value()) {
        return fail(stmt.error());
    }
    return execute(*stmt);
}

Result<void> Session::run(std::string_view text, OutcomeSink& sink) {
    auto stmt = parse(text);
    if (!stmt.has_value()) {
        return fail(stmt.error());
    }
    return execute(*stmt, sink);
}

ConnectionHint Session::connection_hint() const {
    return ConnectionHint{impl_->config.region, !impl_->config.endpoint.empty()};
}

namespace engine {

// Narrow friend accessors into Session::Impl (declared in session_internal.hpp).
// Defined here, where Impl is complete.
IDDBClient& SessionAccess::client(Session& session) {
    return session.impl_->client;
}

std::string SessionAccess::next_alias_prefix(Session& session) {
    return session.impl_->next_alias_prefix();
}

Result<std::optional<SelectRowCursor>> open_select_cursor_for(Session& session,
                                                              const Statement& stmt) {
    // Only a non-EXPLAIN SELECT can stream; everything else (and EXPLAIN) uses
    // the buffering Session::execute path.
    if (stmt.kind() != StatementKind::select_statement || stmt.ast() == nullptr ||
        stmt.ast()->explain) {
        return std::optional<SelectRowCursor>{};
    }
    // An unbound `?` marker must not reach the lazy SELECT cursor either (it
    // would serialize a bogus request); reject it here as the execute overloads
    // do (D-P5-2). Prepared SELECTs are resolved before reaching this path.
    if (count_param_markers(*stmt.ast()) > 0) {
        return fail(Errc::unbound_parameter,
                    "statement has unbound parameter marker(s); use prepare() and bind()");
    }
    const auto& select = std::get<SelectCmd>(stmt.ast()->command);
    // A vector NEAREST search is a single bounded SearchVectors request — keep
    // it on the buffering path (execute_select builds the VectorSearchOutcome,
    // scores and all); the cursor never models vectors.
    if (select.search_vector.has_value()) {
        return std::optional<SelectRowCursor>{};
    }
    auto cursor = open_select_cursor(select, SessionAccess::client(session),
                                     SessionAccess::next_alias_prefix(session));
    if (!cursor.has_value()) {
        return fail(cursor.error());
    }
    return std::optional<SelectRowCursor>{std::move(*cursor)};
}

}  // namespace engine

}  // namespace ddbsh
