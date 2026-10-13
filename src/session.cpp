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
#include "execute/create_table_executor.hpp"
#include "execute/drop_table_executor.hpp"
#include "execute/insert_executor.hpp"
#include "execute/select_executor.hpp"
#include "execute/show_tables_executor.hpp"
#include "expressions/prefix.hpp"
#include "parser/driver.hpp"

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

Result<ExecOutcome> Session::execute(const Statement& statement) {
    switch (statement.kind()) {
        case StatementKind::empty:
            return ExecOutcome{EmptyOutcome{}};
        case StatementKind::show_version:
            // Faithful to the original: SHOW VERSION prints "ddbsh - version
            // <v>\n". In the original this happens inside the parser ACTION for
            // show_version_command (parser.y:646-648: builds no command, prints
            // the line during the parse), NOT via the startup banner
            // (main.cpp:359, which is a separate non-quiet-only path). We move
            // that side effect out of the parser into execute() and carry the
            // line as data (ReportOutcome) so the engine never writes to a
            // stream (P2a-03); the Renderer emits the identical bytes.
            return ExecOutcome{ReportOutcome{{"ddbsh - version " + version()}}};
        case StatementKind::quit:
            return ExecOutcome{StatusOutcome{"QUIT"}};
        case StatementKind::connect: {
            // CONNECT contacts no API (arch §7): it updates the session's
            // region/endpoint (the original CConnectCommand calls
            // setRegionAndEndpoint + reconnect; reconnection is the CLI's job
            // via the injected client, W5/W8). Updating the per-session config
            // keeps the prompt/region correct and emits the CONNECT token.
            const auto& connect = std::get<engine::ConnectCmd>(statement.ast()->command);
            impl_->config.region = connect.region;
            impl_->config.endpoint = connect.endpoint;
            return ExecOutcome{StatusOutcome{"CONNECT"}};
        }
        case StatementKind::select_statement: {
            // EXPLAIN of a SELECT must not contact the service; it renders the
            // serialized request(s) instead, which lands with the EXPLAIN
            // Renderer slice (W6). Until then an explaining SELECT reports
            // not-implemented rather than executing.
            if (statement.ast()->explain) {
                return fail(Errc::not_implemented,
                            "EXPLAIN SELECT is not yet available in this build");
            }
            const auto& select = std::get<engine::SelectCmd>(statement.ast()->command);
            return engine::execute_select(select, impl_->client, impl_->next_alias_prefix());
        }
        case StatementKind::drop_table: {
            // EXPLAIN of a DROP renders the serialized DeleteTable request and
            // must not contact the service (W6); until then an explaining DROP
            // reports not-implemented rather than executing.
            if (statement.ast()->explain) {
                return fail(Errc::not_implemented,
                            "EXPLAIN DROP TABLE is not yet available in this build");
            }
            const auto& drop = std::get<engine::DropTableCmd>(statement.ast()->command);
            return engine::execute_drop_table(drop, impl_->client);
        }
        case StatementKind::insert: {
            // EXPLAIN of an INSERT/REPLACE renders the serialized PutItem
            // request(s) and must not contact the service (W6).
            if (statement.ast()->explain) {
                return fail(Errc::not_implemented,
                            "EXPLAIN INSERT/REPLACE is not yet available in this build");
            }
            const auto& insert = std::get<engine::InsertCmd>(statement.ast()->command);
            return engine::execute_insert(insert, impl_->client,
                                          [this] { return impl_->next_alias_prefix(); });
        }
        case StatementKind::create_table: {
            // EXPLAIN of a CREATE renders the serialized CreateTable request and
            // must not contact the service (W6); until then an explaining CREATE
            // reports not-implemented rather than executing.
            if (statement.ast()->explain) {
                return fail(Errc::not_implemented,
                            "EXPLAIN CREATE TABLE is not yet available in this build");
            }
            const auto& create = std::get<engine::CreateTableCmd>(statement.ast()->command);
            return engine::execute_create_table(create, impl_->client);
        }
        case StatementKind::show_tables: {
            // SHOW TABLES is not explainable (it is a show_command, not in the
            // grammar's `explainable` set — SHOW/CONNECT/QUIT never carry the
            // EXPLAIN flag), so there is no explain path to guard here.
            const auto& show = std::get<engine::ShowTablesCmd>(statement.ast()->command);
            return engine::execute_show_tables(show, impl_->client);
        }
        case StatementKind::describe:
            // Parsed in W2; execution (the full multi-API DESCRIBE report) lands
            // in the next W5 follow-on round. Until then, report a clear
            // not-implemented Error rather than a parse error (D19), so the
            // command surface is observable and tested.
            return fail(Errc::not_implemented,
                        "command parsed but not yet executable in this build");
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

}  // namespace ddbsh
