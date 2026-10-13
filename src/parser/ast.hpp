// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// ast.hpp — the engine-internal parsed command model (the owning AST).
//
// PRIVATE engine header (not installed). The grammar builds these nodes as
// bison semantic values (api.value.type variant) and moves them up the
// productions; ParsedStatement is the parse result the public Statement owns
// (P2a-02 §4.2, Domain C). All nodes are value types with ordinary move
// semantics — no raw owning pointers, no NEW/FREE — so a parse error destroys
// the semantic-value stack with no leak (Domain C/D). Nodes carry only
// SDK-free data (strings/bools/ddbsh::Value), keeping the model free of any
// AWS type (D11).
//
// 2b scope: the control/DDL/show families ported so far. CreateTableCmd,
// InsertCmd, SelectCmd (with the Where expression tree, W3) are added as those
// productions land, each a new alternative in CommandNode.

#ifndef DDBSH_SRC_PARSER_AST_HPP
#define DDBSH_SRC_PARSER_AST_HPP

#include <string>
#include <type_traits>
#include <variant>

#include "ddbsh/statement.hpp"

namespace ddbsh::engine {

/// Strip a matched pair of surrounding double quotes from a lexeme, exactly as
/// the original util/unquote.cpp did: if the string is >= 2 chars and both
/// ends are '"', drop the first and last char; otherwise return it unchanged.
/// NOTE (arch §4 fidelity): the original does NOT collapse SQL-style ""
/// doubling here — the doubled quotes survive in the returned value. Preserve
/// that.
inline std::string unquote(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

/// Blank / comment-only / bare-';' input: executes to nothing.
struct EmptyCmd {};

/// QUIT — exit the shell.
struct QuitCmd {};

/// SHOW VERSION — print the version banner.
struct ShowVersionCmd {};

/// CONNECT <region> [WITH ENDPOINT <endpoint>] — switch region/endpoint.
struct ConnectCmd {
    std::string region;
    std::string endpoint;  ///< empty when no WITH ENDPOINT clause
};

/// SHOW TABLES [BRIEF] [LIKE <template>] — list tables.
struct ShowTablesCmd {
    bool brief = false;
    std::string like;  ///< empty when no LIKE clause
};

/// DESCRIBE <table> — table metadata report.
struct DescribeCmd {
    std::string table;
};

/// DROP TABLE [IF EXISTS] [NOWAIT] <table> — delete a table.
struct DropTableCmd {
    std::string table;
    bool if_exists = false;  ///< IF EXISTS: suppress "not found"
    bool nowait = false;     ///< NOWAIT: do not block until deleted
};

/// The parsed command. Grows one alternative per ported command family.
using CommandNode = std::variant<EmptyCmd, QuitCmd, ShowVersionCmd, ConnectCmd, ShowTablesCmd,
                                 DescribeCmd, DropTableCmd>;

/// Map a CommandNode to its public StatementKind. The variant alternative is
/// the single source of truth; this keeps the public enum in lockstep.
inline StatementKind kind_of(const CommandNode& command) {
    return std::visit(
        [](const auto& node) -> StatementKind {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, EmptyCmd>) {
                return StatementKind::empty;
            } else if constexpr (std::is_same_v<T, QuitCmd>) {
                return StatementKind::quit;
            } else if constexpr (std::is_same_v<T, ShowVersionCmd>) {
                return StatementKind::show_version;
            } else if constexpr (std::is_same_v<T, ConnectCmd>) {
                return StatementKind::connect;
            } else if constexpr (std::is_same_v<T, ShowTablesCmd>) {
                return StatementKind::show_tables;
            } else if constexpr (std::is_same_v<T, DescribeCmd>) {
                return StatementKind::describe;
            } else if constexpr (std::is_same_v<T, DropTableCmd>) {
                return StatementKind::drop_table;
            } else {
                static_assert(sizeof(T) == 0, "unhandled CommandNode alternative");
            }
        },
        command);
}

/// The owning parse result the public Statement holds via unique_ptr.
struct ParsedStatement {
    CommandNode command;
    bool explain = false;  ///< the EXPLAIN prefix was present

    StatementKind kind() const { return kind_of(command); }
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PARSER_AST_HPP
