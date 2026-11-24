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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ddbsh/config.hpp"
#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/result.hpp"
#include "ddbsh/statement.hpp"

namespace ddbsh {

namespace engine {
/// Engine-internal accessor seam (defined in session.cpp, declared in the
/// private src/session_internal.hpp). It grants the engine's own connection
/// layer the narrow access it needs to Session internals (the injected client
/// and the per-statement alias-prefix counter) to open a lazy SELECT cursor
/// (D-P5-1), WITHOUT widening the public, SDK-free embedder surface. It is NOT
/// part of the embedder-facing API.
struct SessionAccess;
}  // namespace engine

/// The result of parsing a whole batch of `;`-separated statements from one
/// string (Session::parse_batch). `statements` holds every statement that
/// parsed successfully, in order; a bare `;` yields an empty statement, and a
/// final statement may omit its `;` (the original string source appended one —
/// arch §4). `error`, when present, is a parse/semantic failure on the
/// statement that follows the ones in `statements`: the already-parsed
/// statements are still returned so the embedder can "execute what it can"
/// before surfacing the error (human-ratified isql-batch model, 2026-10-14).
/// Statement is move-only, so ParsedBatch is move-only.
struct ParsedBatch {
    std::vector<Statement> statements;
    std::optional<Error> error;
};

/// A small, SDK-free, terminal-free snapshot of the session's current
/// connection state, for an embedder that renders a prompt. The engine reports
/// only the raw state (region + whether an endpoint override is in effect); the
/// CLI owns the presentation policy (the `> ` suffix and the `(*)` override
/// marker), keeping terminal/formatting concerns out of the library (tenet 3 /
/// D-2j-2a). The region/endpoint change live when a CONNECT statement executes,
/// so a CLI that re-reads this before each prompt reflects a CONNECT
/// immediately (arch §4/§11, B10).
struct ConnectionHint {
    std::string region;
    bool endpoint_overridden = false;
};

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
    /// this call. If `text` holds several `;`-separated statements, only the
    /// first is returned (use parse_batch for the whole batch); a missing
    /// trailing `;` is tolerated (arch §4).
    Result<Statement> parse(std::string_view text);

    /// Parse a whole batch of `;`-separated statements from one string into a
    /// ParsedBatch (statements in order, plus an optional trailing parse
    /// error). This is the engine's one-shot batch entry point: it parses the
    /// complete string handed in, never pulls more input, and carries nothing
    /// across calls (human-ratified isql-batch model, 2026-10-14). The embedder
    /// (the CLI / golden harness) executes each returned Statement in order and
    /// applies its own stop-on-quit / abort-on-error policy.
    ParsedBatch parse_batch(std::string_view text);

    /// Execute a parsed Statement, driving the IDDBClient as needed. Returns
    /// structured data; the engine writes nothing.
    ///
    /// This BUFFERING overload collects every fragment the statement emits into
    /// one ExecOutcome: zero fragments yield the command's natural single
    /// outcome (e.g. EmptyOutcome), exactly one yields that fragment, and more
    /// than one yields a MultiOutcome carrying them in order (e.g. a multi-row
    /// INSERT). On failure it returns the Error and the fragments produced
    /// before the failure are discarded; an embedder that needs the original's
    /// partial-output-then-error behavior (the CLI) uses the streaming overload
    /// below. (D-2c-12.) The commands that stream per response since P12 keep
    /// their single-outcome shape here: SHOW TABLES, SHOW BACKUPS, SHOW LIMITS
    /// and DESCRIBE return one ReportOutcome; a bulk UPDATE/DELETE returns one
    /// BulkWriteOutcome (RETURN VALUES items included) or, under EXPLAIN, one
    /// ExplainOutcome (D-P12-15).
    Result<ExecOutcome> execute(const Statement& statement);

    /// Execute a parsed Statement, emitting each outcome fragment to `sink` in
    /// production order as it is produced, then returning success or the Error
    /// that stopped execution. Fragments already emitted to the sink before a
    /// failure are the command's partial output (e.g. a multi-row INSERT that
    /// writes k rows then fails emits k WriteOutcome fragments and returns the
    /// Error). The engine writes to no stream; the sink is the embedder's
    /// (D-2c-12). Each fragment is emitted as soon as the API response that
    /// completes it arrives (P12, D-P12-1): one per SELECT page, per written
    /// INSERT row, per SHOW TABLES row, per SHOW BACKUPS page, per DESCRIBE /
    /// SHOW LIMITS response group, per bulk-write RETURN VALUES item, and per
    /// bulk EXPLAIN request group. A fragment can carry no rows: an empty
    /// SELECT emits one SelectOutcome with no items (it carries the
    /// column_hint), and SHOW TABLES / SHOW BACKUPS emit an empty
    /// ReportOutcome to mark a page end (or an empty result); the Renderer
    /// renders these as nothing. After the last fragment, success or failure,
    /// this calls sink.end_statement() exactly once (D-P12-16); an exception
    /// thrown by the sink's emit() propagates and skips it.
    Result<void> execute(const Statement& statement, OutcomeSink& sink);

    /// Convenience: parse then execute (buffering).
    Result<ExecOutcome> run(std::string_view text);

    /// Convenience: parse then execute (streaming to `sink`).
    Result<void> run(std::string_view text, OutcomeSink& sink);

    /// Report the session's current connection state for an embedder's prompt
    /// (see ConnectionHint). Reads the per-session config, which a CONNECT
    /// statement updates live, so a CLI re-reading this before each prompt
    /// reflects a CONNECT immediately (B10 / D-2j-2a). Terminal-free: the
    /// engine returns state, not a rendered string.
    ConnectionHint connection_hint() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Narrow engine-internal access to impl_ for the connection layer's lazy
    // SELECT cursor (D-P5-1). SessionAccess is defined in session.cpp where
    // Impl is complete; this friendship does not widen the public surface.
    friend struct engine::SessionAccess;
};

}  // namespace ddbsh

#endif  // DDBSH_SESSION_HPP
