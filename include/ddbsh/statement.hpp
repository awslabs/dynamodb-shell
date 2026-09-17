// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// statement.hpp — a parsed, self-contained command ready to execute.
//
// Session::parse(text) -> Result<Statement>; Session::execute(const
// Statement&) -> Result<ExecOutcome>. A Statement holds NO pointer into
// scanner/parser memory (P2a-02 §7.2): all grammar fragments are copied/moved
// out of the parse, so a Statement outlives the per-parse flex/bison handles.
//
// 2a scope: the walking-skeleton grammar recognizes SHOW VERSION, QUIT, and
// empty input. The full command model (owning AST via unique_ptr, EXPLAIN
// flag) lands in 2b/2c per P2a-02 §4.

#ifndef DDBSH_STATEMENT_HPP
#define DDBSH_STATEMENT_HPP

namespace ddbsh {

/// The kind of statement parsed. Grows into the full command catalog in 2b+.
enum class StatementKind {
    empty,         ///< blank / comment-only input
    show_version,  ///< SHOW VERSION
    quit,          ///< QUIT / EXIT
};

/// A self-contained parsed statement. Trivially copyable/movable in 2a; when
/// the full AST lands (2b/2c) it gains an owning unique_ptr payload and this
/// stays move-only.
struct Statement {
    StatementKind kind = StatementKind::empty;
    bool explain = false;  ///< set when the EXPLAIN prefix is seen (2b+)
};

}  // namespace ddbsh

#endif  // DDBSH_STATEMENT_HPP
