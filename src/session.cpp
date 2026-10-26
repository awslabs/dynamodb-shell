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

#include <utility>

#include "ddbsh/version.hpp"
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

namespace ddbsh {

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
    struct BufferingSink : OutcomeSink {
        std::vector<ExecOutcome> parts;
        void emit(const ExecOutcome& fragment) override { parts.push_back(fragment); }
    } sink;

    auto streamed = execute(statement, sink);
    if (!streamed.has_value()) {
        return fail(streamed.error());
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
            impl_->config.region = connect.region;
            impl_->config.endpoint = connect.endpoint;
            (void)impl_->client.connect(connect.region, connect.endpoint);
            return emit_single(ExecOutcome{StatusOutcome{"CONNECT"}});
        }
        case StatementKind::select_statement: {
            const auto& select = std::get<engine::SelectCmd>(statement.ast()->command);
            if (statement.ast()->explain) {
                // EXPLAIN renders the serialized GetItem/Query/Scan request
                // (the planner still resolves the key schema; it does not read
                // data). W6.
                return emit_single(engine::execute_select_explain(select, impl_->client,
                                                                  impl_->next_alias_prefix()));
            }
            return emit_single(
                engine::execute_select(select, impl_->client, impl_->next_alias_prefix()));
        }
        case StatementKind::drop_table: {
            const auto& drop = std::get<engine::DropTableCmd>(statement.ast()->command);
            if (statement.ast()->explain) {
                // EXPLAIN renders the serialized DeleteTable request (IF EXISTS
                // still probes existence; a missing table emits nothing). W6.
                return emit_single(engine::execute_drop_table_explain(drop, impl_->client));
            }
            return emit_single(engine::execute_drop_table(drop, impl_->client));
        }
        case StatementKind::insert: {
            const auto& insert = std::get<engine::InsertCmd>(statement.ast()->command);
            if (statement.ast()->explain) {
                // EXPLAIN renders the serialized PutItem request(s), one per
                // row (the key schema is still resolved for the condition). W6.
                return emit_single(engine::execute_insert_explain(
                    insert, impl_->client, [this] { return impl_->next_alias_prefix(); }));
            }
            // Streaming: emit one WriteOutcome per written row BEFORE a failing
            // row's Error, so partial output survives the failure (AI-4 /
            // D-2c-12) — the original printed each row's token then the error.
            return engine::execute_insert_streaming(
                insert, impl_->client, [this] { return impl_->next_alias_prefix(); }, sink);
        }
        case StatementKind::update_statement: {
            const auto& update = std::get<engine::UpdateCmd>(statement.ast()->command);
            return engine::execute_update(
                update, impl_->client, [this] { return impl_->next_alias_prefix(); },
                statement.ast()->explain, sink);
        }
        case StatementKind::del: {
            const auto& del = std::get<engine::DeleteCmd>(statement.ast()->command);
            return engine::execute_delete(
                del, impl_->client, [this] { return impl_->next_alias_prefix(); },
                statement.ast()->explain, sink);
        }
        case StatementKind::transaction: {
            const auto& txn = std::get<engine::TransactCmd>(statement.ast()->command);
            return engine::execute_transact(
                txn, impl_->client, [this] { return impl_->next_alias_prefix(); },
                statement.ast()->explain, sink);
        }
        case StatementKind::create_table: {
            const auto& create = std::get<engine::CreateTableCmd>(statement.ast()->command);
            if (statement.ast()->explain) {
                // EXPLAIN renders the serialized CreateTable request plus the
                // CREATE token the original prints even when explaining. W6.
                return emit_single(engine::execute_create_table_explain(create, impl_->client));
            }
            return emit_single(engine::execute_create_table(create, impl_->client));
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
                    return emit_single(engine::execute_alter_table_explain(alter, impl_->client));
                }
                return emit_single(engine::execute_alter_table(alter, impl_->client));
            }
            if (std::holds_alternative<engine::AlterTtlCmd>(command)) {
                // SET TTL: DescribeTimeToLive + UpdateTimeToLive (the executor
                // owns the probe-first / already-disabled short-circuit / EXPLAIN
                // logic, D-2e-7), so pass the explain flag through.
                const auto& ttl = std::get<engine::AlterTtlCmd>(command);
                return emit_single(engine::execute_alter_ttl(ttl, impl_->client, explain));
            }
            if (std::holds_alternative<engine::AlterPitrCmd>(command)) {
                // SET PITR: UpdateContinuousBackups (the executor owns the EXPLAIN
                // `UpdateContinuousBackupsRequest(...)` label quirk, D-2e-8/-11).
                const auto& pitr = std::get<engine::AlterPitrCmd>(command);
                return emit_single(engine::execute_alter_pitr(pitr, impl_->client, explain));
            }
            // REPLICA: UpdateTable with one ReplicationGroupUpdate (D-2e-9); the
            // executor owns the EXPLAIN `UpdateTable(...)` block.
            const auto& replica = std::get<engine::AlterReplicaCmd>(command);
            return emit_single(engine::execute_alter_replica(replica, impl_->client, explain));
        }
        case StatementKind::show_tables: {
            // SHOW TABLES is not explainable (it is a show_command, not in the
            // grammar's `explainable` set — SHOW/CONNECT/QUIT never carry the
            // EXPLAIN flag), so there is no explain path to guard here.
            const auto& show = std::get<engine::ShowTablesCmd>(statement.ast()->command);
            return emit_single(engine::execute_show_tables(show, impl_->client));
        }
        case StatementKind::describe:
            if (statement.ast()->explain) {
                // EXPLAIN renders the four serialized DescribeTable/
                // DescribeTimeToLive/DescribeContinuousBackups/
                // DescribeTableReplicaAutoScaling requests; no service call. W6.
                return emit_single(engine::execute_describe_explain(
                    std::get<engine::DescribeCmd>(statement.ast()->command), impl_->client));
            }
            return emit_single(engine::execute_describe(
                std::get<engine::DescribeCmd>(statement.ast()->command), impl_->client));
        case StatementKind::create_backup:
            // BACKUP TABLE ... CALLED ... — a ddl command, so explainable.
            return emit_single(engine::execute_create_backup(
                std::get<engine::CreateBackupCmd>(statement.ast()->command), impl_->client,
                statement.ast()->explain));
        case StatementKind::drop_backup:
            return emit_single(engine::execute_drop_backup(
                std::get<engine::DropBackupCmd>(statement.ast()->command), impl_->client,
                statement.ast()->explain));
        case StatementKind::describe_backup:
            return emit_single(engine::execute_describe_backup(
                std::get<engine::DescribeBackupCmd>(statement.ast()->command), impl_->client,
                statement.ast()->explain));
        case StatementKind::restore:
            return emit_single(
                engine::execute_restore(std::get<engine::RestoreCmd>(statement.ast()->command),
                                        impl_->client, statement.ast()->explain));
        case StatementKind::show_backups:
            // SHOW commands are not explainable (grammar: show_command is not in
            // the explainable set, matching the oracle — EXPLAIN SHOW ... is a
            // syntax error), so there is no explain path here.
            return emit_single(engine::execute_show_backups(
                std::get<engine::ShowBackupsCmd>(statement.ast()->command), impl_->client));
        case StatementKind::show_limits:
            // The `Region:` line is sourced from the session's current region
            // (D-2f-9), threaded as a parameter — not from the client seam.
            return emit_single(engine::execute_show_limits(
                std::get<engine::ShowLimitsCmd>(statement.ast()->command), impl_->client,
                impl_->config.region));
        case StatementKind::show_create_table:
            return emit_single(engine::execute_show_create_table(
                std::get<engine::ShowCreateTableCmd>(statement.ast()->command), impl_->client));
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

}  // namespace ddbsh
