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
#include "parser/driver.hpp"

namespace ddbsh {

struct Session::Impl {
    SessionConfig config;
    IDDBClient& client;  // borrowed; the embedder guarantees it outlives us

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
        case StatementKind::show_tables:
        case StatementKind::describe:
        case StatementKind::drop_table:
        case StatementKind::create_table:
        case StatementKind::insert:
            // Parsed in W2; execution (behind IDDBClient) lands in W4/W5. Until
            // then, report a clear not-implemented Error rather than a parse
            // error (D19), so the command surface is observable and tested.
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
