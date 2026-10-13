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
// SDK-free mirrors of the DynamoDB table-definition shapes the original built
// directly as Aws:: model objects (parser.y create_table_command). The W4/W6
// executor/renderer translate these to the CreateTable request; keeping them
// SDK-free holds the D11 boundary.

/// A key attribute's scalar type (CREATE TABLE attribute definitions / the
/// original ScalarAttributeType). N=number, S=string, B=binary.
enum class ScalarType { n, s, b };

/// One attribute definition: name + scalar type.
struct AttributeDef {
    std::string name;
    ScalarType type = ScalarType::s;
};

/// A key schema: a required HASH attribute and an optional RANGE attribute.
/// The original always emits [HASH, RANGE] order regardless of written order
/// (parser.y key_schema reorders), which this shape reproduces.
struct KeySchema {
    std::string hash;
    std::optional<std::string> range;
};

/// Table/GSI billing mode (the original BillingMode).
enum class BillingMode { pay_per_request, provisioned };

/// On-demand (PAY_PER_REQUEST) max request units. Both nullopt => a bare
/// `BILLING MODE ON DEMAND` (empty OnDemandThroughput); both set => an explicit
/// `THROUGHPUT (r,w)`; both -1 => `THROUGHPUT UNLIMITED` (matching the
/// original's -1 sentinel, arch §8).
struct OnDemandThroughput {
    std::optional<long long> max_read_request_units;
    std::optional<long long> max_write_request_units;
};

/// Billing mode plus the throughput that goes with it. For `provisioned`, rcu/
/// wcu carry the ProvisionedThroughput; for `pay_per_request`, `od` carries the
/// (optional) OnDemandThroughput.
struct BillingModeAndThroughput {
    BillingMode mode = BillingMode::pay_per_request;
    long long rcu = 0;
    long long wcu = 0;
    OnDemandThroughput od;
};

/// WARM THROUGHPUT (r RCU, w WCU) — the original WarmThroughput.
struct WarmThroughput {
    long long read_units_per_second = 0;
    long long write_units_per_second = 0;
};

/// Index projection type (the original ProjectionType).
enum class ProjectionType { all, keys_only, include };

/// An index projection: type plus the non-key attributes for INCLUDE.
struct Projection {
    ProjectionType type = ProjectionType::all;
    std::vector<std::string> non_key_attributes;  ///< only for `include`
};

/// A global secondary index definition.
struct Gsi {
    std::string name;
    KeySchema key_schema;
    Projection projection;
    std::optional<BillingModeAndThroughput> billing;
    std::optional<WarmThroughput> warm;
};

/// A local secondary index definition.
struct Lsi {
    std::string name;
    KeySchema key_schema;
    Projection projection;
};

/// Stream view type (the original StreamViewType).
enum class StreamViewType { keys_only, new_image, old_image, new_and_old_images };

/// STREAM (...) | STREAM DISABLED. `view_type` is meaningful only when enabled.
struct StreamSpec {
    bool enabled = false;
    StreamViewType view_type = StreamViewType::keys_only;
};

/// Server-side-encryption key type (the original SSEType).
enum class SseType { kms, aes256 };

/// SSE DISABLED | SSE (KEYID <id> KMS|AES256). `kms_master_key_id`/`sse_type`
/// are set only when enabled.
struct SseSpec {
    bool enabled = false;
    std::optional<std::string> kms_master_key_id;
    std::optional<SseType> sse_type;
};

/// Table class (the original TableClass).
enum class TableClass { standard, standard_infrequent_access };

/// A table tag (key:value).
struct Tag {
    std::string key;
    std::string value;
};

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
using CommandNode = std::variant<EmptyCmd, QuitCmd, ShowVersionCmd, ConnectCmd, ShowTablesCmd,
                                 DescribeCmd, DropTableCmd, CreateTableCmd, InsertCmd, SelectCmd>;

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
