// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// statement.hpp — a parsed, self-contained command ready to execute.
//
// Session::parse(text) -> Result<Statement>; Session::execute(const
// Statement&) -> Result<ExecOutcome>. A Statement OWNS its parsed AST and holds
// NO pointer into scanner/parser memory (P2a-02 §7.2): all grammar fragments
// are moved out of the parse into the AST, so a Statement outlives the
// per-parse flex/bison handles.
//
// Ownership model (P2a-02 §4.2): the Statement owns the command/AST by
// std::unique_ptr inside a PIMPL (engine::ParsedStatement). This keeps the
// public header SDK-free and free of flex/bison types (D11); the full AST node
// types live in the engine-internal header src/parser/ast.hpp. The public
// surface is just kind()/explain() plus a library-internal AST accessor the
// executor uses. Statement is move-only (it owns a unique_ptr).
//
// 2b scope: the grammar builds the owning AST for the control/DDL/show command
// families ported so far (empty, SHOW VERSION, QUIT, CONNECT, SHOW TABLES,
// DESCRIBE, DROP TABLE). CREATE TABLE / INSERT / SELECT and their EXPLAIN land
// in subsequent W2/W3 commits on this same model.

#ifndef DDBSH_STATEMENT_HPP
#define DDBSH_STATEMENT_HPP

#include <memory>

namespace ddbsh {

namespace engine {
struct ParsedStatement;  // defined in src/parser/ast.hpp (engine-internal)
}

/// The kind of statement parsed. Derived from the owned AST node; grows into
/// the full command catalog as families port.
enum class StatementKind {
    empty,             ///< blank / comment-only / bare-';' input
    show_version,      ///< SHOW VERSION
    quit,              ///< QUIT
    connect,           ///< CONNECT <region> [WITH ENDPOINT <endpoint>]
    show_tables,       ///< SHOW TABLES [BRIEF] [LIKE <template>]
    describe,          ///< DESCRIBE <table>
    drop_table,        ///< DROP TABLE [IF EXISTS] [NOWAIT] <table>
    create_table,      ///< CREATE TABLE [IF NOT EXISTS] [NOWAIT] <table> ( ... ) ...
    alter_table,       ///< ALTER TABLE <table> ... (UpdateTable / SET TTL / SET PITR / REPLICA)
    insert,            ///< INSERT INTO / REPLACE INTO <table> ( cols ) VALUES ( ... )
    select_statement,  ///< SELECT [CONSISTENT] <proj|*> FROM <table>[.<index>] [WHERE ...]
    update_statement,  ///< UPDATE / UPSERT <table>[.<index>] SET|REMOVE ... [WHERE ...]
    del,               ///< DELETE FROM <table>[.<index>] [WHERE ...]
    help,              ///< HELP [topic ...] — print usage (no API call)
    transaction,       ///< BEGIN; ... (COMMIT|ROLLBACK); — TransactGet/WriteItems
};

/// A self-contained parsed statement. Move-only: it owns the parsed AST via a
/// unique_ptr PIMPL. A default-constructed Statement is the empty statement.
class Statement {
public:
    /// The empty statement (blank / comment-only input).
    Statement();

    /// Takes ownership of a parsed AST. Library-internal: the parser builds the
    /// ParsedStatement and hands it here. A null `ast` yields the empty
    /// statement.
    explicit Statement(std::unique_ptr<engine::ParsedStatement> ast);

    ~Statement();

    Statement(Statement&&) noexcept;
    Statement& operator=(Statement&&) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    /// The kind of command this statement carries (empty if default-built).
    StatementKind kind() const;

    /// True when the EXPLAIN prefix was present (arch §7); EXPLAIN prints the
    /// serialized API request(s) instead of executing.
    bool explain() const;

    /// Library-internal accessor to the owned AST (nullptr for the empty
    /// statement). Not part of the embedder-facing API surface; the engine's
    /// executor uses it to read the parsed command. Returns a borrowed,
    /// non-owning pointer valid for this Statement's lifetime.
    const engine::ParsedStatement* ast() const noexcept { return ast_.get(); }

private:
    std::unique_ptr<engine::ParsedStatement> ast_;
};

}  // namespace ddbsh

#endif  // DDBSH_STATEMENT_HPP
