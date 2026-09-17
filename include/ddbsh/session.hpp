// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// session.hpp — the engine entry point (replaces the CDDBSh singleton, T1).
//
// A Session is a value/object an embedder constructs with a SessionConfig and
// a borrowed IDDBClient. There are no process globals and no getInstance();
// multiple Sessions coexist safely (D3 reentrancy — see the multithreaded
// reentrancy test). The flex/bison scanner+parser state and any Aws:: types
// are hidden behind the PIMPL (D11), so this header stays SDK-free and
// terminal-free.
//
// Lifetime contract (P2a-02 §7):
//   * The embedder guarantees the IDDBClient outlives the Session.
//   * Scanner/parser handles are strictly per-parse() and never escape it; a
//     returned Statement holds no pointer into parse memory.
//   * The destructor is defined in the .cpp where Impl is complete (PIMPL).

#ifndef DDBSH_SESSION_HPP
#define DDBSH_SESSION_HPP

#include <memory>
#include <string_view>

#include "ddbsh/config.hpp"
#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "ddbsh/statement.hpp"

namespace ddbsh {

/// The engine session: parse text into a Statement, execute a Statement
/// against the injected IDDBClient. One Session is NOT thread-safe for
/// concurrent use (it holds the per-session alias-prefix counter and schema
/// cache); the contract is N threads / N sessions (P2a-02 §7.3).
class Session {
public:
    /// @param cfg     per-session configuration (region/endpoint).
    /// @param client  borrowed DynamoDB client; must outlive this Session.
    Session(SessionConfig cfg, IDDBClient& client);
    ~Session();

    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Parse one whole command (the EXPLAIN prefix, if present, is recorded on
    /// the Statement). Scanner/parser state is constructed and destroyed within
    /// this call.
    Result<Statement> parse(std::string_view text);

    /// Execute a parsed Statement, driving the IDDBClient as needed. Returns
    /// structured data; the engine writes nothing.
    Result<ExecOutcome> execute(const Statement& statement);

    /// Convenience: parse then execute.
    Result<ExecOutcome> run(std::string_view text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddbsh

#endif  // DDBSH_SESSION_HPP
