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

/// EXISTS / NOT EXISTS prefix on a SELECT (arch §7, the original's
/// exists_command). Meaningful ONLY inside a write transaction, where an
/// EXISTS/NOT-EXISTS SELECT becomes a TransactWriteItems ConditionCheck
/// (attribute_exists / attribute_not_exists on the item's key or projected
/// attributes). Standalone (outside a transaction), the original runs the
/// SELECT normally and ignores this flag (select.cpp CSelectCommand::run never
/// consults m_exists; only txwrite() does) — the reimplementation reproduces
/// that: a SelectCmd with exists_mode != none executed as StatementKind::
/// select_statement runs as an ordinary SELECT (D-2d-3).
enum class ExistsMode { none, exists, not_exists };

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
    ExistsMode exists_mode = ExistsMode::none;  ///< set by EXISTS/NOT EXISTS (write txn only)
};

// ---- UPDATE / UPSERT / DELETE (W5 / 2c) ------------------------------------

/// One UPDATE SET assignment (port of CUpdateSetElement). A value type with
/// ordinary move semantics. Serialized through the engine SymbolTable to the
/// `#lhs = <rhs>` forms (arch §6):
///   * plain value:        `#lhs = :v`         (rhs_value set, rhs_attribute "")
///   * attribute copy:     `#lhs = #b`         (rhs_attribute set)
///   * arithmetic:         `#lhs = #b +/- :v`  (rhs_attribute + op + rhs_value)
///   * if_not_exists:      `#lhs = if_not_exists(#p, :v)`  (if_not_exists_*)
/// NOTE on if_not_exists (fidelity correction): arch §12 #5 claims
/// `if_not_exists` is a dead token (not in the keyword table). That is FALSE —
/// it is in both the original's keyword table (oracle/parser/keywords.cpp) and
/// ours (src/parser/lexer.l), the original grammar wires it, and the golden
/// `iut-ifne-*.exp` prove the original emits `SET #a = if_not_exists(#a,:v)`.
/// Supporting it is therefore FIDELITY (tenet 1/6), not the human-gated
/// divergence D-2c-15 assumed; it is implemented here so the iut- family
/// matches the golden byte-for-byte.
struct UpdateSetElement {
    std::string lhs;                           ///< lee_lhs path (dotted/subscripted ok)
    std::string op;                            ///< "" | "+" | "-" (arithmetic operator)
    std::optional<Value> rhs_value;            ///< `= :v` and the arithmetic operand
    std::string rhs_attribute;                 ///< `= #b` (attribute copy; empty if none)
    std::string if_not_exists_lhs;             ///< if_not_exists(#p, :v): the path #p
    std::optional<Value> if_not_exists_value;  ///< if_not_exists(#p, :v): the :v
};

/// UPDATE/UPSERT mutation kind. SET and REMOVE are grammar-exclusive.
enum class Mutation { set, remove };

/// UPDATE / UPSERT (one node, `upsert` flag — mirrors the original's single
/// CUpdateCommand). UPDATE prepends `attribute_exists(pk)` to the write
/// condition; UPSERT does not (and can create). SET xor REMOVE. `where` is null
/// when there is no WHERE clause (UPSERT always carries a full-key WHERE; a
/// bare UPDATE with no WHERE enumerates every item). Move-only (owns a
/// unique_ptr<Where>).
struct UpdateCmd {
    std::string table;
    std::optional<std::string> index;  ///< `table.index` read-routing form
    Mutation mutation = Mutation::set;
    std::vector<UpdateSetElement> set;  ///< when mutation == set
    std::vector<std::string> remove;    ///< when mutation == remove (paths)
    std::unique_ptr<Where> where;       ///< null => no WHERE (update all)
    bool upsert = false;                ///< UPSERT omits attribute_exists(pk)
    std::optional<RateLimit> ratelimit;
    ReturnValue return_value = ReturnValue::none;
};

/// DELETE FROM <table>[.<index>] [WHERE ...]. No WHERE => delete all (full scan
/// + per-item DeleteItem). `.index` routes the enumeration read through the
/// index; the per-item writes hit the base table. Move-only (owns a
/// unique_ptr<Where>).
struct DeleteCmd {
    std::string table;
    std::optional<std::string> index;
    std::unique_ptr<Where> where;  ///< null => delete all
    std::optional<RateLimit> ratelimit;
    ReturnValue return_value = ReturnValue::none;
};

struct TransactCmd;  // defined below (recursive: holds std::vector<CommandNode>)

/// The parsed command. Grows one alternative per ported command family.
using CommandNode = std::variant<EmptyCmd, QuitCmd, ShowVersionCmd, ConnectCmd, ShowTablesCmd,
                                 DescribeCmd, DropTableCmd, CreateTableCmd, InsertCmd, SelectCmd,
                                 UpdateCmd, DeleteCmd, HelpCmd, TransactCmd>;

/// A transaction: BEGIN; <homogeneous inner statements> (COMMIT|ROLLBACK);
/// (arch §7). `access` is fixed by the grammar (a get-sequence vs a
/// write-sequence are distinct non-terminals; a mixed body is a syntax error —
/// there is no mixed production). `rollback` records the terminator (ROLLBACK =
/// true, COMMIT = false). `items` are the inner command nodes in parse order:
/// SelectCmd for a read transaction; InsertCmd / UpdateCmd / DeleteCmd /
/// SelectCmd(exists_mode != none) for a write transaction. Empty `items` with
/// `rollback == true` is the no-op `begin; rollback;` (renders nothing, itxt-1).
///
/// Recursion: `items` is a std::vector<CommandNode> where CommandNode is the
/// variant that also lists TransactCmd — the same incomplete-type-in-std::vector
/// allowance MultiOutcome relies on (D-2c assumption, gcc14/libstdc++ C++23).
/// Move-only in practice because inner SelectCmd/UpdateCmd/DeleteCmd own a
/// unique_ptr<Where>.
struct TransactCmd {
    enum class Access { read, write };
    Access access = Access::read;
    bool rollback = false;           ///< ROLLBACK terminator (vs COMMIT)
    std::vector<CommandNode> items;  ///< inner commands, in parse order
};

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
            } else if constexpr (std::is_same_v<T, UpdateCmd>) {
                return StatementKind::update_statement;
            } else if constexpr (std::is_same_v<T, DeleteCmd>) {
                return StatementKind::del;
            } else if constexpr (std::is_same_v<T, HelpCmd>) {
                return StatementKind::help;
            } else if constexpr (std::is_same_v<T, TransactCmd>) {
                return StatementKind::transaction;
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
