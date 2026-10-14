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

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "ddbsh/statement.hpp"
#include "ddbsh/table_schema.hpp"
#include "ddbsh/value.hpp"
#include "expressions/where.hpp"

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

/// Parse a base-10 whole number lexeme to long long WITHOUT throwing (the
/// engine never throws across its boundary — D12). Mirrors the original's
/// atoll() for throughput/capacity literals: the lexer already guarantees the
/// lexeme matches `-?[1-9][0-9]*` or `0`, so strtoll's error paths are not
/// reachable for in-range values; out-of-range saturates rather than throwing.
inline long long parse_ll(const std::string& s) {
    return std::strtoll(s.c_str(), nullptr, 10);
}

/// Parse a numeric lexeme to double WITHOUT throwing (mirrors the original's
/// atof for ratelimit RCU/WCU literals). The lexer guarantees a numeric lexeme,
/// so strtod's error path is not reachable for in-range values.
inline double parse_double(const std::string& s) {
    return std::strtod(s.c_str(), nullptr);
}

/// Blank / comment-only / bare-';' input: executes to nothing.
struct EmptyCmd {};

/// A HELP topic. The original parser had one grammar production per HELP form
/// (parser.y: `help_command`), each printing a fixed usage string during the
/// parse and yielding no command (arch §5). This reimplementation keeps the
/// shell terminal-free: HELP parses to a HelpCmd carrying the topic, and
/// execute() emits the matching usage text as a ReportOutcome (same pattern as
/// SHOW VERSION, which also moved its parser-time printf into execute()). The
/// per-topic text lives in src/parser/help_text.cpp, byte-for-byte faithful to
/// the original so output matches `ddbsh-old` exactly (tenet 1, D-B).
enum class HelpTopic {
    general,            ///< HELP
    explain,            ///< HELP EXPLAIN
    begin,              ///< HELP BEGIN
    commit,             ///< HELP COMMIT
    rollback,           ///< HELP ROLLBACK
    connect,            ///< HELP CONNECT
    drop,               ///< HELP DROP
    drop_table,         ///< HELP DROP TABLE
    describe,           ///< HELP DESCRIBE
    show,               ///< HELP SHOW
    show_tables,        ///< HELP SHOW TABLES
    create,             ///< HELP CREATE
    create_table,       ///< HELP CREATE TABLE
    alter_table,        ///< HELP ALTER TABLE
    show_limits,        ///< HELP SHOW LIMITS
    show_backups,       ///< HELP SHOW BACKUPS
    describe_backup,    ///< HELP DESCRIBE BACKUP
    drop_backup,        ///< HELP DROP BACKUP
    backup,             ///< HELP BACKUP
    restore,            ///< HELP RESTORE
    select_statement,   ///< HELP SELECT
    insert,             ///< HELP INSERT
    replace,            ///< HELP REPLACE
    update,             ///< HELP UPDATE
    upsert,             ///< HELP UPSERT
    del,                ///< HELP DELETE
    show_version,       ///< HELP SHOW VERSION
    show_create_table,  ///< HELP SHOW CREATE TABLE
};

/// HELP [topic ...] — print usage text, contact no API (arch §7). Carries only
/// the resolved topic; the usage text is produced at render time.
struct HelpCmd {
    HelpTopic topic = HelpTopic::general;
};

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

// ---- CREATE TABLE supporting value types -----------------------------------
// The SDK-free table-definition value types (AttributeDef, KeySchema, billing/
// throughput, Gsi/Lsi, stream/sse/class/tags, ...) live in the public header
// ddbsh/table_schema.hpp so the parse result (CreateTableCmd below) and the
// client seam (ddbsh::CreateTableIn) share ONE definition. They are introduced
// into ddbsh::engine by the using-declarations below, so the grammar and the
// AST refer to them unqualified (or as engine::X) exactly as before while the
// single source of truth is the public header. (The W5 executor copies these
// into a CreateTableIn; the W6 renderer serializes them for EXPLAIN.)
using ddbsh::AttributeDef;
using ddbsh::BillingMode;
using ddbsh::BillingModeAndThroughput;
using ddbsh::Gsi;
using ddbsh::KeySchema;
using ddbsh::Lsi;
using ddbsh::OnDemandThroughput;
using ddbsh::Projection;
using ddbsh::ProjectionType;
using ddbsh::ScalarType;
using ddbsh::SseSpec;
using ddbsh::SseType;
using ddbsh::StreamSpec;
using ddbsh::StreamViewType;
using ddbsh::TableClass;
using ddbsh::Tag;
using ddbsh::WarmThroughput;

/// CREATE TABLE [IF NOT EXISTS] [NOWAIT] <name> ( attr type, ... ) PRIMARY KEY
/// ( ... ) [billing] [warm] [gsi] [lsi] [stream] [sse] [class] [deletion] [tags]
struct CreateTableCmd {
    std::string table_name;
    bool if_not_exists = false;
    bool nowait = false;
    std::vector<AttributeDef> attribute_definitions;
    KeySchema key_schema;
    std::optional<BillingModeAndThroughput> billing;  ///< nullopt => default on-demand
    std::optional<WarmThroughput> warm;
    std::vector<Gsi> gsis;
    std::vector<Lsi> lsis;
    std::optional<StreamSpec> stream;
    std::optional<SseSpec> sse;
    TableClass table_class = TableClass::standard;
    bool deletion_protection = false;
    std::vector<Tag> tags;
};

// ---- INSERT / REPLACE (W2 part 2) ------------------------------------------

/// A write ratelimit (WITH RATELIMIT (...)). Captured for fidelity; execution
/// of the token-bucket limiter is deferred to a later sub-phase (D19 defers
/// rate-limiting), so the presence here just records the clause.
struct RateLimit {
    double rcu = 0;
    double wcu = 0;
};

/// RETURN VALUES target (the original ReturnValue). INSERT forces `none`;
/// REPLACE may request one. Non-`none` execution lands with return-values
/// support in a later sub-phase.
enum class ReturnValue { none, all_old, all_new, updated_old, updated_new };

/// INSERT INTO / REPLACE INTO <table> ( cols ) VALUES ( row ) [, ( row ) ...]
/// INSERT (insert=true) adds a conditional attribute_not_exists(pk); REPLACE
/// (insert=false) is unconditional (arch §7). One CInsertCommand backs both in
/// the original; one node backs both here. Each row is a positional list of
/// Values matching `columns` by index.
struct InsertCmd {
    std::string table_name;
    std::vector<std::string> columns;
    std::vector<ValueList> rows;
    bool insert = true;
    std::optional<RateLimit> ratelimit;
    ReturnValue return_value = ReturnValue::none;
};

// ---- SELECT (W3 part 2) ----------------------------------------------------

/// A SELECT target: a base table, optionally qualified by `.index` (arch §7,
/// §8). The original carried this as a 1-or-2 element Aws::Vector<Aws::String>;
/// a named struct makes the index-vs-no-index distinction explicit.
struct TableReference {
    std::string table;
    std::optional<std::string> index;  ///< `table.index` form; nullopt for base table
};

/// RETURN CONSUMED CAPACITY target (the original ReturnConsumedCapacity). The
/// default is `none`; a ratelimit forces `total` at execution (arch §8). Mixing
/// an explicit RETURN CONSUMED CAPACITY with a ratelimit is a parse-time error
/// (the original's "You cannot specify both ..." message).
enum class ReturnConsumedCapacity { none, total, indexes };

/// SELECT [CONSISTENT] <projection|*> FROM <table>[.<index>] [WHERE ...]
/// [RETURN CONSUMED CAPACITY ...] [WITH RATELIMIT ...]. An empty `projection`
/// means `*` (project all attributes), exactly as the original treated a
/// missing projection list. `where` is null when there is no WHERE clause.
/// Move-only: it owns the WHERE expression tree via unique_ptr<Where>.
struct SelectCmd {
    bool consistent = false;
    std::vector<std::string> projection;  ///< empty => `*` (all attributes)
    std::string table;
    std::optional<std::string> index;
    std::unique_ptr<Where> where;  ///< null => no WHERE clause
    ReturnConsumedCapacity rcc = ReturnConsumedCapacity::none;
    std::optional<RateLimit> ratelimit;
};

/// The parsed command. Grows one alternative per ported command family.
using CommandNode =
    std::variant<EmptyCmd, QuitCmd, ShowVersionCmd, ConnectCmd, ShowTablesCmd, DescribeCmd,
                 DropTableCmd, CreateTableCmd, InsertCmd, SelectCmd, HelpCmd>;

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
            } else if constexpr (std::is_same_v<T, CreateTableCmd>) {
                return StatementKind::create_table;
            } else if constexpr (std::is_same_v<T, InsertCmd>) {
                return StatementKind::insert;
            } else if constexpr (std::is_same_v<T, SelectCmd>) {
                return StatementKind::select_statement;
            } else if constexpr (std::is_same_v<T, HelpCmd>) {
                return StatementKind::help;
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
