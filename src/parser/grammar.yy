/* Copyright 2026 ddbsh reimplementation contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * grammar.yy — the engine grammar (bison C++ lalr1.cc skeleton, D3).
 *
 * Phase 2b, W1 declared the COMPLETE token set the tokenizer produces (arch
 * §4), matching the original parser.y token list one-for-one. W2 adds the
 * owning AST and the in-scope command PRODUCTIONS. This commit ports the
 * control/DDL/show families that need no WHERE expression tree or nested
 * values: empty / bare-';', SHOW VERSION, QUIT, CONNECT [WITH ENDPOINT],
 * SHOW TABLES [BRIEF][LIKE], DESCRIBE, DROP TABLE [IF EXISTS][NOWAIT], and the
 * EXPLAIN prefix over ddl commands. CREATE TABLE / INSERT / SELECT (with the
 * expression tree, W3) are added as subsequent alternatives on this same
 * model. Tokens declared-but-unused in this intermediate state are expected
 * and are not -Werror (the generated TU is compiled -Wno-error).
 *
 * The generated parser is a class (ddbsh::engine::Parser) with a reentrant
 * interface; its header stays private behind Session::Impl (D11). Each command
 * reduces to an owning engine::CommandNode (a variant of value-type AST nodes,
 * src/parser/ast.hpp), MOVED up the productions — no NEW/FREE, no raw owning
 * pointer, and a parse error destroys the semantic-value stack with no leak
 * (P2a-02 Domain C/D). The public Statement owns the resulting ParsedStatement.
 */

%require "3.7"
%language "c++"
%skeleton "lalr1.cc"

%define api.namespace {ddbsh::engine}
%define api.parser.class {Parser}
%define api.value.type variant
%define api.token.constructor
%define parse.error verbose

%code requires {
    #include <string>
    #include "ddbsh/statement.hpp"
    #include "parser/ast.hpp"
    namespace ddbsh::engine { class Driver; }
}

%param { ddbsh::engine::Driver& drv }

%code {
    #include "parser/driver.hpp"
    // Concrete WHERE-tree node types the SELECT reductions construct (W3).
    // ast.hpp (via %code requires) already pulls the LogicalExpression base and
    // Where; these headers add the leaf/compound nodes used only in actions.
    #include "expressions/logical_and.hpp"
    #include "expressions/logical_comparison.hpp"
    #include "expressions/logical_filter_predicates.hpp"
    #include "expressions/logical_or.hpp"
    #include "expressions/logical_range_predicates.hpp"
    // The reentrant flex scanner entry (defined in the generated lexer). The
    // bridge ddbsh_lex(Driver&) forwards to it with the Driver's scanner
    // handle; bison's yylex is mapped to that bridge.
    ddbsh::engine::Parser::symbol_type ddbsh_lex(ddbsh::engine::Driver& drv);
    #define yylex ddbsh_lex
}

%token END 0        "end of input"
%token SEMICOLON    ";"

/* ---- Keyword tokens (valueless; the scanner returns them by kind) ---------
 * One-for-one with the original parser.y keyword list; order here is the
 * original's declaration order. The scanner's keyword table (lexer.l) maps the
 * matched identifier (case-insensitively) to the corresponding token.
 */
%token K_ACCESS
%token K_ADD
%token K_AES256
%token K_ALL
%token K_ALTER
%token K_AND
%token K_AS
%token K_ATTRIBUTE_EXISTS
%token K_ATTRIBUTE_NOT_EXISTS
%token K_ATTRIBUTE_TYPE
%token K_ATTRIBUTE_TYPE_BINARY
%token K_ATTRIBUTE_TYPE_NUMBER
%token K_ATTRIBUTE_TYPE_STRING
%token K_BACKUP
%token K_BACKUPS
%token K_BEGIN
%token K_BEGINS_WITH
%token K_BETWEEN
%token K_BILLING
%token K_BOTH
%token K_BRIEF
%token K_CALLED
%token K_CAPACITY
%token K_CHECK
%token K_CLASS
%token K_COMMIT
%token K_CONNECT
%token K_CONSISTENT
%token K_CONSUMED
%token K_CONTAINS
%token K_CREATE
%token K_DELETE
%token K_DELETION
%token K_DEMAND
%token K_DESCRIBE
%token K_DISABLED
%token K_DROP
%token K_ENABLED
%token K_ENDPOINT
%token K_EXISTS
%token K_EXPLAIN
%token K_FALSE
%token K_FROM
%token K_GLOBAL
%token K_GSI
%token K_HASH
%token K_HELP
%token K_IF
%token K_IF_NOT_EXISTS
%token K_IMAGE
%token K_IMAGES
%token K_IN
%token K_INCLUDE
%token K_INDEX
%token K_INDEXES
%token K_INFREQUENT
%token K_INSERT
%token K_INTO
%token K_IS
%token K_KEY
%token K_KEYID
%token K_KEYS
%token K_KMS
%token K_LIKE
%token K_LIMIT
%token K_LIMITS
%token K_LSI
%token K_MODE
%token K_NEW
%token K_NOT
%token K_NOWAIT
%token K_NULL
%token K_NUMBER
%token K_OLD
%token K_ON
%token K_ONLY
%token K_OR
%token K_PITR
%token K_PRIMARY
%token K_PROJECTING
%token K_PROTECTION
%token K_PROVISIONED
%token K_QUIT
%token K_RANGE
%token K_RATELIMIT
%token K_RCU
%token K_REMOVE
%token K_REPLACE
%token K_REPLICA
%token K_RESTORE
%token K_RETURN
%token K_ROLLBACK
%token K_SELECT
%token K_SET
%token K_SHOW
%token K_SIZE
%token K_SSE
%token K_STANDARD
%token K_STREAM
%token K_TABLE
%token K_TABLES
%token K_TAGS
%token K_THROUGHPUT
%token K_TO
%token K_TOTAL
%token K_TRUE
%token K_TTL
%token K_UNLIMITED
%token K_UPDATE
%token K_UPDATED
%token K_UPSERT
%token K_VALUES
%token K_VERSION
%token K_WARM
%token K_WCU
%token K_WHERE
%token K_WITH

/* ---- Value-carrying tokens (std::string lexeme) --------------------------
 * The original kept the raw matched text (quotes included for quoted strings);
 * unquote()/number handling happens in grammar actions (W2). We preserve that:
 * the scanner stores the verbatim lexeme.
 */
%token <std::string> T_DQUOTED_STRING
%token <std::string> T_EQUAL
%token <std::string> T_FLOATING_POINT_NUMBER
%token <std::string> T_GREATER_THAN
%token <std::string> T_GREATER_THAN_OR_EQUAL
%token <std::string> T_LESS_THAN
%token <std::string> T_LESS_THAN_OR_EQUAL
%token <std::string> T_NOT_EQUAL
%token <std::string> T_TIMESTAMP
%token <std::string> T_UNQUOTED_STRING
%token <std::string> T_WHOLE_NUMBER

/* ---- Nonterminal semantic value types ------------------------------------
 * Commands reduce to an owning CommandNode (a variant of value-type AST nodes,
 * src/parser/ast.hpp); the parser MOVES them up the productions (no raw owning
 * pointers, no NEW/FREE — P2a-02 Domain C/D). Leaf fragments are std::string
 * (identifiers/paths, unquoted) and bool (optional flag clauses).
 */
%type <ddbsh::engine::CommandNode> command explainable
%type <ddbsh::engine::CommandNode> control_command ddl_command show_command
%type <ddbsh::engine::CommandNode> connect_command quit_command
%type <ddbsh::engine::CommandNode> drop_table_command describe_command
%type <ddbsh::engine::CommandNode> show_tables_command show_version_command
%type <ddbsh::engine::CommandNode> create_table_command
%type <ddbsh::engine::CommandNode> dml_command write_command
%type <ddbsh::engine::CommandNode> insert_command replace_command

%type <std::string> string region_name connect_endpoint like_clause
%type <bool> brief_clause if_exists_clause nowait_clause

/* ---- CREATE TABLE nonterminal value types (W2 part 2) --------------------
 * The original built these directly as Aws:: model objects in the grammar
 * actions; we build the SDK-free engine::* mirrors (src/parser/ast.hpp) and
 * MOVE them up the productions (no raw owning pointers — P2a-02 Domain C/D).
 */
%type <ddbsh::engine::ScalarType> attribute_type
%type <ddbsh::engine::AttributeDef> attribute_name_type
%type <std::vector<ddbsh::engine::AttributeDef>> attribute_name_type_list
%type <ddbsh::engine::KeySchema> key_schema primary_key
%type <ddbsh::engine::BillingModeAndThroughput> billing_mode_and_throughput
%type <std::optional<ddbsh::engine::BillingModeAndThroughput>> optional_billing_mode_and_throughput
%type <ddbsh::engine::WarmThroughput> warm_throughput
%type <std::optional<ddbsh::engine::WarmThroughput>> optional_warm_throughput
%type <std::vector<ddbsh::engine::Gsi>> gsi_list gsi_specification
%type <ddbsh::engine::Gsi> gsi
%type <std::vector<ddbsh::engine::Lsi>> lsi_list lsi_specification
%type <ddbsh::engine::Lsi> lsi
%type <ddbsh::engine::Projection> index_projection
%type <std::vector<std::string>> index_projection_list
%type <ddbsh::engine::StreamSpec> stream_specification
%type <std::optional<ddbsh::engine::StreamSpec>> optional_stream_specification
%type <ddbsh::engine::StreamViewType> stream_view_type
%type <ddbsh::engine::SseSpec> sse_specification
%type <std::optional<ddbsh::engine::SseSpec>> optional_sse_specification
%type <ddbsh::engine::TableClass> table_class optional_table_class
%type <bool> deletion_protection optional_deletion_protection if_not_exists_clause
%type <std::vector<ddbsh::engine::Tag>> table_tags table_tags_list optional_table_tags
%type <ddbsh::engine::Tag> table_tag

/* ---- INSERT / REPLACE + value literals (W2 part 2) -----------------------
 * The original built Aws:: AttributeValue objects in the grammar; we build the
 * SDK-free ddbsh::Value mirror (include/ddbsh/value.hpp). lee_rhs is the full
 * value literal (incl. empty []/{}) reused by SELECT's RHS (W3); insert_value
 * is the INSERT subset (no top-level empty collection), faithfully distinct as
 * in the original. */
%type <std::string> table_name insert_column
%type <std::vector<std::string>> insert_column_list
%type <ddbsh::Value> insert_value lee_rhs map_element_list
%type <ddbsh::ValueList> insert_values lee_rhs_list
%type <std::vector<ddbsh::ValueList>> value_list
%type <std::optional<ddbsh::engine::RateLimit>> optional_ratelimit ratelimit
%type <ddbsh::engine::ReturnValue> optional_return_value

/* ---- SELECT + WHERE expression tree (W3 part 2) --------------------------
 * The original built a CSelectCommand plus a CWhere/CLogicalExpression tree in
 * the grammar; we build the SDK-free engine::SelectCmd carrying a
 * unique_ptr<Where> (which owns the unique_ptr<LogicalExpression> tree). The
 * expression nodes are MOVED up the productions (RAII ownership, no NEW/FREE,
 * no raw owning pointers — P2a-02 Domain C/D); a parse error unwinds the
 * semantic-value stack and frees the partially-built tree with no leak. */
%type <ddbsh::engine::CommandNode> get_command select_command
%type <bool> optional_consistent
%type <std::vector<std::string>> select_projection select_attribute_list
%type <std::string> select_attribute lee_lhs comparison_op
%type <ddbsh::engine::TableReference> table_optional_index_name
%type <std::unique_ptr<ddbsh::engine::LogicalExpression>> logical_expression
%type <std::unique_ptr<ddbsh::engine::LogicalExpression>> logical_expression_element
%type <std::unique_ptr<ddbsh::engine::Where>> optional_where_clause where_clause
%type <ddbsh::engine::ReturnConsumedCapacity> optional_return_consumed_capacity

/* Precedence/associativity for the WHERE expression grammar, matching the
 * original parser.y (low -> high): OR < AND < NOT < BETWEEN < . [ ]. Required
 * to resolve the ambiguous logical_expression recursion the same way the
 * original did. */
%left K_OR
%left K_AND
%left K_NOT
%left K_BETWEEN
%left '.' '[' ']'

%%

/* ONE `;`-terminated statement per parser invocation, then YYACCEPT (exactly
 * the original's run loop: its yyparse YYACCEPTs after each top-level command
 * and is re-entered for the next statement — arch §3/§5). The Driver keeps a
 * single persistent scanner alive for the whole batch string and constructs a
 * fresh Parser per statement over it (Driver::parse_next); the YYACCEPT below
 * returns control to that loop at each statement boundary WITHOUT requiring the
 * end-of-input token, so a batch like `a; b;` parses as two statements rather
 * than erroring on the second (the W7 multi-statement blocker). This is engine-
 * internal to a single one-shot parse_batch() call: no input is pulled across
 * calls and no state carries over (human-ratified isql-batch model, 2026-10-14
 * — supersedes the InputSource/StatementStream design).
 *
 * The trailing ';' is OPTIONAL on the final statement (the `command` /
 * `K_EXPLAIN explainable` arms with no SEMICOLON). This reproduces the
 * original string input source, which appended a ';' if the `-c` payload
 * lacked one (arch §4) — many golden `-c` commands omit the terminator
 * (e.g. `drop table if exists icd1`, `explain select ... from st1`, and the
 * trailing `select` of a multi-statement batch). The engine never mutates its
 * input to add the ';'; the grammar accepts the end-of-input in its place. The
 * `command SEMICOLON` vs `command` (and explainable) pair is unambiguous: the
 * state after `command` shifts on ';' and reduces (to a complete statement)
 * only on end-of-input, so there is no shift/reduce conflict and no lookahead
 * token is consumed past the terminator.
 *
 * The EXPLAIN prefix wraps a ddl/dml command (arch §7); it sets the AST's
 * explain flag rather than mutating a shared command object (P2a-02 §4.2). On a
 * syntax error, bison records the Error via error() and recovers to the next
 * ';'; the Driver then reports failure for that statement. */
input:
      %empty
        { drv.set_empty_result(); }
    | SEMICOLON
        { drv.set_empty_result(); YYACCEPT; }
    | command SEMICOLON
        { drv.set_result(std::move($1), false); YYACCEPT; }
    | command
        { drv.set_result(std::move($1), false); YYACCEPT; }
    | K_EXPLAIN explainable SEMICOLON
        { drv.set_result(std::move($2), true); YYACCEPT; }
    | K_EXPLAIN explainable
        { drv.set_result(std::move($2), true); YYACCEPT; }
    | error SEMICOLON
        { /* error already recorded by error(); recover to ';' */ YYACCEPT; }
    ;

command:
      control_command    { $$ = std::move($1); }
    | ddl_command        { $$ = std::move($1); }
    | dml_command        { $$ = std::move($1); }
    | show_command       { $$ = std::move($1); }
    ;

/* EXPLAIN accepts ddl/dml commands only (as the original did); SHOW/CONNECT/
 * QUIT are not explainable. dml is the write family (INSERT/REPLACE) and the
 * read family (SELECT via get_command, W3). */
explainable:
      ddl_command        { $$ = std::move($1); }
    | dml_command        { $$ = std::move($1); }
    ;

control_command:
      connect_command    { $$ = std::move($1); }
    | quit_command       { $$ = std::move($1); }
    ;

ddl_command:
      drop_table_command { $$ = std::move($1); }
    | describe_command   { $$ = std::move($1); }
    | create_table_command { $$ = std::move($1); }
    ;

/* dml for the write family (INSERT/REPLACE) and the read family (SELECT via
 * get_command, W3). Both join `explainable` so EXPLAIN works over them. */
dml_command:
      get_command        { $$ = std::move($1); }
    | write_command      { $$ = std::move($1); }
    ;

get_command:
      select_command     { $$ = std::move($1); }
    ;

write_command:
      insert_command     { $$ = std::move($1); }
    | replace_command    { $$ = std::move($1); }
    ;

show_command:
      show_tables_command   { $$ = std::move($1); }
    | show_version_command  { $$ = std::move($1); }
    ;

quit_command:
      K_QUIT
        { $$ = ddbsh::engine::QuitCmd{}; }
    ;

show_version_command:
      K_SHOW K_VERSION
        { $$ = ddbsh::engine::ShowVersionCmd{}; }
    ;

connect_command:
      K_CONNECT region_name connect_endpoint
        { $$ = ddbsh::engine::ConnectCmd{std::move($2), std::move($3)}; }
    ;

connect_endpoint:
      %empty                       { $$ = std::string{}; }
    | K_WITH K_ENDPOINT string     { $$ = std::move($3); }
    ;

drop_table_command:
      K_DROP K_TABLE if_exists_clause nowait_clause string
        { $$ = ddbsh::engine::DropTableCmd{std::move($5), $3, $4}; }
    ;

describe_command:
      K_DESCRIBE string
        { $$ = ddbsh::engine::DescribeCmd{std::move($2)}; }
    ;

show_tables_command:
      K_SHOW K_TABLES brief_clause like_clause
        { $$ = ddbsh::engine::ShowTablesCmd{$3, std::move($4)}; }
    ;

/* ---- CREATE TABLE (W2 part 2) --------------------------------------------
 * A faithful port of the original create_table_command and its clause
 * productions (parser.y). The trailing ';' is consumed by `input` (not here),
 * so every $N is one lower than the original. Each clause builds the SDK-free
 * engine::* mirror; optionals carry std::nullopt when the clause is absent,
 * which the W4/W6 executor/renderer translate to the CreateTable request. The
 * on-demand default (no billing clause) is applied at execution, exactly as the
 * original did (create_table.cpp), not here. */
create_table_command:
      K_CREATE K_TABLE if_not_exists_clause nowait_clause string
      '(' attribute_name_type_list ')'
      primary_key optional_billing_mode_and_throughput optional_warm_throughput
      gsi_list lsi_list optional_stream_specification optional_sse_specification
      optional_table_class optional_deletion_protection optional_table_tags
        {
            ddbsh::engine::CreateTableCmd cmd;
            cmd.if_not_exists = $3;
            cmd.nowait = $4;
            cmd.table_name = std::move($5);
            cmd.attribute_definitions = std::move($7);
            cmd.key_schema = std::move($9);
            cmd.billing = std::move($10);
            cmd.warm = std::move($11);
            cmd.gsis = std::move($12);
            cmd.lsis = std::move($13);
            cmd.stream = std::move($14);
            cmd.sse = std::move($15);
            cmd.table_class = $16;
            cmd.deletion_protection = $17;
            cmd.tags = std::move($18);
            $$ = std::move(cmd);
        }
    ;

attribute_name_type_list:
      attribute_name_type
        {
            $$ = std::vector<ddbsh::engine::AttributeDef>{};
            $$.push_back(std::move($1));
        }
    | attribute_name_type_list ',' attribute_name_type
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

attribute_name_type:
      string attribute_type
        { $$ = ddbsh::engine::AttributeDef{std::move($1), $2}; }
    ;

attribute_type:
      K_ATTRIBUTE_TYPE_NUMBER  { $$ = ddbsh::engine::ScalarType::n; }
    | K_ATTRIBUTE_TYPE_STRING  { $$ = ddbsh::engine::ScalarType::s; }
    | K_ATTRIBUTE_TYPE_BINARY  { $$ = ddbsh::engine::ScalarType::b; }
    ;

primary_key:
      K_PRIMARY K_KEY key_schema  { $$ = std::move($3); }
    ;

/* The original always emits [HASH, RANGE] order regardless of the written
 * order; KeySchema{hash, optional range} captures that canonically. */
key_schema:
      '(' string K_HASH ')'
        { $$ = ddbsh::engine::KeySchema{std::move($2), std::nullopt}; }
    | '(' string K_HASH ',' string K_RANGE ')'
        { $$ = ddbsh::engine::KeySchema{std::move($2), std::move($5)}; }
    | '(' string K_RANGE ',' string K_HASH ')'
        { $$ = ddbsh::engine::KeySchema{std::move($5), std::move($2)}; }
    ;

billing_mode_and_throughput:
      K_BILLING K_MODE K_ON K_DEMAND
        {
            ddbsh::engine::BillingModeAndThroughput b;
            b.mode = ddbsh::engine::BillingMode::pay_per_request;
            $$ = std::move(b);
        }
    | K_BILLING K_MODE K_ON K_DEMAND K_THROUGHPUT '(' T_WHOLE_NUMBER K_RCU ',' T_WHOLE_NUMBER K_WCU ')'
        {
            ddbsh::engine::BillingModeAndThroughput b;
            b.mode = ddbsh::engine::BillingMode::pay_per_request;
            b.od.max_read_request_units = ddbsh::engine::parse_ll($7);
            b.od.max_write_request_units = ddbsh::engine::parse_ll($10);
            $$ = std::move(b);
        }
    | K_BILLING K_MODE K_ON K_DEMAND K_THROUGHPUT '(' T_WHOLE_NUMBER K_WCU ',' T_WHOLE_NUMBER K_RCU ')'
        {
            ddbsh::engine::BillingModeAndThroughput b;
            b.mode = ddbsh::engine::BillingMode::pay_per_request;
            b.od.max_write_request_units = ddbsh::engine::parse_ll($7);
            b.od.max_read_request_units = ddbsh::engine::parse_ll($10);
            $$ = std::move(b);
        }
    | K_BILLING K_MODE K_ON K_DEMAND K_THROUGHPUT K_UNLIMITED
        {
            ddbsh::engine::BillingModeAndThroughput b;
            b.mode = ddbsh::engine::BillingMode::pay_per_request;
            b.od.max_read_request_units = -1;
            b.od.max_write_request_units = -1;
            $$ = std::move(b);
        }
    | K_BILLING K_MODE K_PROVISIONED '(' T_WHOLE_NUMBER K_RCU ',' T_WHOLE_NUMBER K_WCU ')'
        {
            ddbsh::engine::BillingModeAndThroughput b;
            b.mode = ddbsh::engine::BillingMode::provisioned;
            b.rcu = ddbsh::engine::parse_ll($5);
            b.wcu = ddbsh::engine::parse_ll($8);
            $$ = std::move(b);
        }
    | K_BILLING K_MODE K_PROVISIONED '(' T_WHOLE_NUMBER K_WCU ',' T_WHOLE_NUMBER K_RCU ')'
        {
            ddbsh::engine::BillingModeAndThroughput b;
            b.mode = ddbsh::engine::BillingMode::provisioned;
            b.wcu = ddbsh::engine::parse_ll($5);
            b.rcu = ddbsh::engine::parse_ll($8);
            $$ = std::move(b);
        }
    ;

optional_billing_mode_and_throughput:
      billing_mode_and_throughput  { $$ = std::move($1); }
    | %empty                       { $$ = std::nullopt; }
    ;

warm_throughput:
      K_WARM K_THROUGHPUT '(' T_WHOLE_NUMBER K_RCU ',' T_WHOLE_NUMBER K_WCU ')'
        { $$ = ddbsh::engine::WarmThroughput{ddbsh::engine::parse_ll($4),
                                             ddbsh::engine::parse_ll($7)}; }
    | K_WARM K_THROUGHPUT '(' T_WHOLE_NUMBER K_WCU ',' T_WHOLE_NUMBER K_RCU ')'
        { $$ = ddbsh::engine::WarmThroughput{ddbsh::engine::parse_ll($7),
                                             ddbsh::engine::parse_ll($4)}; }
    ;

optional_warm_throughput:
      warm_throughput  { $$ = std::move($1); }
    | %empty           { $$ = std::nullopt; }
    ;

gsi_list:
      K_GSI '(' gsi_specification ')'  { $$ = std::move($3); }
    | %empty                           { $$ = std::vector<ddbsh::engine::Gsi>{}; }
    ;

gsi_specification:
      gsi
        {
            $$ = std::vector<ddbsh::engine::Gsi>{};
            $$.push_back(std::move($1));
        }
    | gsi_specification ',' gsi
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

gsi:
      string K_ON key_schema index_projection optional_billing_mode_and_throughput optional_warm_throughput
        {
            ddbsh::engine::Gsi g;
            g.name = std::move($1);
            g.key_schema = std::move($3);
            g.projection = std::move($4);
            g.billing = std::move($5);
            g.warm = std::move($6);
            $$ = std::move(g);
        }
    ;

lsi_list:
      K_LSI '(' lsi_specification ')'  { $$ = std::move($3); }
    | %empty                           { $$ = std::vector<ddbsh::engine::Lsi>{}; }
    ;

lsi_specification:
      lsi
        {
            $$ = std::vector<ddbsh::engine::Lsi>{};
            $$.push_back(std::move($1));
        }
    | lsi_specification ',' lsi
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

lsi:
      string K_ON key_schema index_projection
        {
            ddbsh::engine::Lsi l;
            l.name = std::move($1);
            l.key_schema = std::move($3);
            l.projection = std::move($4);
            $$ = std::move(l);
        }
    ;

index_projection:
      K_PROJECTING K_ALL
        { $$ = ddbsh::engine::Projection{ddbsh::engine::ProjectionType::all, {}}; }
    | K_PROJECTING K_KEYS K_ONLY
        { $$ = ddbsh::engine::Projection{ddbsh::engine::ProjectionType::keys_only, {}}; }
    | K_PROJECTING K_INCLUDE '(' index_projection_list ')'
        { $$ = ddbsh::engine::Projection{ddbsh::engine::ProjectionType::include, std::move($4)}; }
    ;

index_projection_list:
      string
        {
            $$ = std::vector<std::string>{};
            $$.push_back(std::move($1));
        }
    | index_projection_list ',' string
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

stream_specification:
      K_STREAM '(' stream_view_type ')'
        { $$ = ddbsh::engine::StreamSpec{true, $3}; }
    | K_STREAM K_DISABLED
        { $$ = ddbsh::engine::StreamSpec{false, ddbsh::engine::StreamViewType::keys_only}; }
    ;

optional_stream_specification:
      stream_specification  { $$ = std::move($1); }
    | %empty                { $$ = std::nullopt; }
    ;

stream_view_type:
      K_KEYS K_ONLY    { $$ = ddbsh::engine::StreamViewType::keys_only; }
    | K_NEW K_IMAGE    { $$ = ddbsh::engine::StreamViewType::new_image; }
    | K_OLD K_IMAGE    { $$ = ddbsh::engine::StreamViewType::old_image; }
    | K_BOTH K_IMAGES  { $$ = ddbsh::engine::StreamViewType::new_and_old_images; }
    ;

optional_sse_specification:
      sse_specification  { $$ = std::move($1); }
    | %empty             { $$ = std::nullopt; }
    ;

sse_specification:
      K_SSE K_DISABLED
        { $$ = ddbsh::engine::SseSpec{false, std::nullopt, std::nullopt}; }
    | K_SSE '(' K_KEYID string K_KMS ')'
        { $$ = ddbsh::engine::SseSpec{true, std::move($4), ddbsh::engine::SseType::kms}; }
    | K_SSE '(' K_KEYID string K_AES256 ')'
        { $$ = ddbsh::engine::SseSpec{true, std::move($4), ddbsh::engine::SseType::aes256}; }
    ;

table_class:
      K_TABLE K_CLASS K_STANDARD
        { $$ = ddbsh::engine::TableClass::standard; }
    | K_TABLE K_CLASS K_STANDARD K_INFREQUENT K_ACCESS
        { $$ = ddbsh::engine::TableClass::standard_infrequent_access; }
    ;

optional_table_class:
      table_class  { $$ = $1; }
    | %empty       { $$ = ddbsh::engine::TableClass::standard; }
    ;

deletion_protection:
      K_DELETION K_PROTECTION K_ENABLED   { $$ = true; }
    | K_DELETION K_PROTECTION K_DISABLED  { $$ = false; }
    ;

optional_deletion_protection:
      deletion_protection  { $$ = $1; }
    | %empty               { $$ = false; }
    ;

table_tags:
      K_TAGS '(' table_tags_list ')'  { $$ = std::move($3); }
    ;

optional_table_tags:
      table_tags  { $$ = std::move($1); }
    | %empty      { $$ = std::vector<ddbsh::engine::Tag>{}; }
    ;

table_tags_list:
      table_tag
        {
            $$ = std::vector<ddbsh::engine::Tag>{};
            $$.push_back(std::move($1));
        }
    | table_tags_list ',' table_tag
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

table_tag:
      string ':' string
        { $$ = ddbsh::engine::Tag{std::move($1), std::move($3)}; }
    ;

/* ---- INSERT / REPLACE (W2 part 2) ----------------------------------------
 * The trailing ';' is consumed by `input`, so each $N is one lower than the
 * original. INSERT forces ReturnValue::none and is conditional at execution;
 * REPLACE is unconditional and may carry a RETURN VALUES target. */
insert_command:
      K_INSERT K_INTO table_name '(' insert_column_list ')' K_VALUES value_list optional_ratelimit
        {
            ddbsh::engine::InsertCmd cmd;
            cmd.table_name = std::move($3);
            cmd.columns = std::move($5);
            cmd.rows = std::move($8);
            cmd.insert = true;
            cmd.ratelimit = std::move($9);
            cmd.return_value = ddbsh::engine::ReturnValue::none;
            $$ = std::move(cmd);
        }
    ;

replace_command:
      K_REPLACE K_INTO table_name '(' insert_column_list ')' K_VALUES value_list optional_ratelimit optional_return_value
        {
            ddbsh::engine::InsertCmd cmd;
            cmd.table_name = std::move($3);
            cmd.columns = std::move($5);
            cmd.rows = std::move($8);
            cmd.insert = false;
            cmd.ratelimit = std::move($9);
            cmd.return_value = $10;
            $$ = std::move(cmd);
        }
    ;

table_name:
      string  { $$ = std::move($1); }
    ;

insert_column_list:
      insert_column
        {
            $$ = std::vector<std::string>{};
            $$.push_back(std::move($1));
        }
    | insert_column_list ',' insert_column
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

insert_column:
      string  { $$ = std::move($1); }
    ;

value_list:
      '(' insert_values ')'
        {
            $$ = std::vector<ddbsh::ValueList>{};
            $$.push_back(std::move($2));
        }
    | value_list ',' '(' insert_values ')'
        { $1.push_back(std::move($4)); $$ = std::move($1); }
    ;

insert_values:
      insert_value
        {
            $$ = ddbsh::ValueList{};
            $$.push_back(std::move($1));
        }
    | insert_values ',' insert_value
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

/* insert_value is the INSERT value subset (no top-level empty []/{}); nested
 * empty collections are still reachable via lee_rhs inside a list/map, exactly
 * as the original. Numbers keep their EXACT lexeme (N), strings are unquoted. */
insert_value:
      T_WHOLE_NUMBER            { $$ = ddbsh::Value::number_value(std::move($1)); }
    | T_FLOATING_POINT_NUMBER   { $$ = ddbsh::Value::number_value(std::move($1)); }
    | T_DQUOTED_STRING          { $$ = ddbsh::Value::string_value(ddbsh::engine::unquote($1)); }
    | '[' lee_rhs_list ']'
        {
            ddbsh::Value v;
            v.type = ddbsh::Value::Type::L;
            v.list = std::move($2);
            $$ = std::move(v);
        }
    | K_TRUE                    { $$ = ddbsh::Value::bool_value(true); }
    | K_FALSE                   { $$ = ddbsh::Value::bool_value(false); }
    | K_NULL                    { $$ = ddbsh::Value::null_value(); }
    | '{' map_element_list '}'  { $$ = std::move($2); }
    ;

/* lee_rhs is the full value literal (adds top-level empty []/{} over
 * insert_value); it is the RHS of WHERE comparisons and the element type of
 * lists/map values (W3 reuses it for SELECT). */
lee_rhs:
      T_WHOLE_NUMBER            { $$ = ddbsh::Value::number_value(std::move($1)); }
    | T_FLOATING_POINT_NUMBER   { $$ = ddbsh::Value::number_value(std::move($1)); }
    | T_DQUOTED_STRING          { $$ = ddbsh::Value::string_value(ddbsh::engine::unquote($1)); }
    | '[' lee_rhs_list ']'
        {
            ddbsh::Value v;
            v.type = ddbsh::Value::Type::L;
            v.list = std::move($2);
            $$ = std::move(v);
        }
    | K_TRUE                    { $$ = ddbsh::Value::bool_value(true); }
    | K_FALSE                   { $$ = ddbsh::Value::bool_value(false); }
    | '{' map_element_list '}'  { $$ = std::move($2); }
    | K_NULL                    { $$ = ddbsh::Value::null_value(); }
    | '[' ']'
        {
            ddbsh::Value v;
            v.type = ddbsh::Value::Type::L;
            $$ = std::move(v);
        }
    | '{' '}'
        {
            ddbsh::Value v;
            v.type = ddbsh::Value::Type::M;
            $$ = std::move(v);
        }
    ;

lee_rhs_list:
      lee_rhs
        {
            $$ = ddbsh::ValueList{};
            $$.push_back(std::move($1));
        }
    | lee_rhs_list ',' lee_rhs
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

/* map_element_list builds a Value of type M, keys in source order (ValueMap is
 * insertion-ordered; the Renderer sorts at format time, arch §9). */
map_element_list:
      string ':' lee_rhs
        {
            ddbsh::Value v;
            v.type = ddbsh::Value::Type::M;
            v.map.emplace_back(std::move($1), std::move($3));
            $$ = std::move(v);
        }
    | map_element_list ',' string ':' lee_rhs
        {
            $1.map.emplace_back(std::move($3), std::move($5));
            $$ = std::move($1);
        }
    ;

ratelimit:
      K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_RCU ',' T_WHOLE_NUMBER K_WCU ')'
        { $$ = ddbsh::engine::RateLimit{ddbsh::engine::parse_double($4),
                                        ddbsh::engine::parse_double($7)}; }
    | K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_WCU ',' T_WHOLE_NUMBER K_RCU ')'
        { $$ = ddbsh::engine::RateLimit{ddbsh::engine::parse_double($7),
                                        ddbsh::engine::parse_double($4)}; }
    | K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_RCU ')'
        { $$ = ddbsh::engine::RateLimit{ddbsh::engine::parse_double($4), 0}; }
    | K_WITH K_RATELIMIT '(' T_WHOLE_NUMBER K_WCU ')'
        { $$ = ddbsh::engine::RateLimit{0, ddbsh::engine::parse_double($4)}; }
    ;

optional_ratelimit:
      ratelimit  { $$ = std::move($1); }
    | %empty     { $$ = std::nullopt; }
    ;

optional_return_value:
      K_RETURN K_VALUES K_ALL K_OLD       { $$ = ddbsh::engine::ReturnValue::all_old; }
    | K_RETURN K_VALUES K_ALL K_NEW       { $$ = ddbsh::engine::ReturnValue::all_new; }
    | K_RETURN K_VALUES K_UPDATED K_OLD   { $$ = ddbsh::engine::ReturnValue::updated_old; }
    | K_RETURN K_VALUES K_UPDATED K_NEW   { $$ = ddbsh::engine::ReturnValue::updated_new; }
    | %empty                              { $$ = ddbsh::engine::ReturnValue::none; }
    ;

/* ---- SELECT (W3 part 2) --------------------------------------------------
 * The trailing ';' is consumed by `input`, so each $N is one lower than the
 * original select_command. The original rejected combining an explicit RETURN
 * CONSUMED CAPACITY with a ratelimit (both force ReturnConsumedCapacity on the
 * request); we reproduce that byte-exact error via a semantic error on the
 * Driver, which makes the parse fail (the statement is not executed). */
select_command:
      K_SELECT optional_consistent select_projection K_FROM table_optional_index_name
      optional_where_clause optional_return_consumed_capacity optional_ratelimit
        {
            if ($8.has_value() &&
                $7 != ddbsh::engine::ReturnConsumedCapacity::none) {
                drv.set_semantic_error(
                    "You cannot specify both a ratelimit and a return_consumed_capacity.");
                // A placeholder result; the recorded error makes parse() fail
                // regardless, so this node is never executed.
                $$ = ddbsh::engine::EmptyCmd{};
            } else {
                ddbsh::engine::SelectCmd cmd;
                cmd.consistent = $2;
                cmd.projection = std::move($3);
                cmd.table = std::move($5.table);
                cmd.index = std::move($5.index);
                cmd.where = std::move($6);
                cmd.rcc = $7;
                cmd.ratelimit = std::move($8);
                $$ = std::move(cmd);
            }
        }
    ;

optional_consistent:
      %empty         { $$ = false; }
    | K_CONSISTENT   { $$ = true; }
    ;

/* `*` is an empty projection (project all attributes), exactly as the original
 * treated a missing list. */
select_projection:
      '*'                     { $$ = std::vector<std::string>{}; }
    | select_attribute_list   { $$ = std::move($1); }
    ;

select_attribute_list:
      select_attribute
        {
            $$ = std::vector<std::string>{};
            $$.push_back(std::move($1));
        }
    | select_attribute_list ',' select_attribute
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

select_attribute:
      lee_lhs  { $$ = std::move($1); }
    ;

table_optional_index_name:
      table_name '.' table_name
        { $$ = ddbsh::engine::TableReference{std::move($1), std::move($3)}; }
    | table_name
        { $$ = ddbsh::engine::TableReference{std::move($1), std::nullopt}; }
    ;

optional_where_clause:
      %empty         { $$ = nullptr; }
    | where_clause   { $$ = std::move($1); }
    ;

where_clause:
      K_WHERE logical_expression
        { $$ = std::make_unique<ddbsh::engine::Where>(std::move($2)); }
    ;

/* The WHERE expression tree. NOT toggles the child's negate flag; parentheses
 * mark a paren group (which blocks the AND/OR flatten factories); AND/OR use
 * the flatten factories (make_and/make_or). */
logical_expression:
      logical_expression_element
        { $$ = std::move($1); }
    | K_NOT logical_expression
        { $2->negate(); $$ = std::move($2); }
    | '(' logical_expression ')'
        { $2->set_paren_group(); $$ = std::move($2); }
    | logical_expression K_AND logical_expression
        { $$ = ddbsh::engine::LogicalAnd::make_and(std::move($1), std::move($3)); }
    | logical_expression K_OR logical_expression
        { $$ = ddbsh::engine::LogicalOr::make_or(std::move($1), std::move($3)); }
    ;

logical_expression_element:
      lee_lhs comparison_op lee_rhs
        {
            $$ = std::make_unique<ddbsh::engine::LogicalComparison>(
                std::move($1), $2, std::move($3));
        }
    | lee_lhs K_IS K_NULL
        {
            // IS NULL desugars to `= <null>` (arch §6).
            $$ = std::make_unique<ddbsh::engine::LogicalComparison>(
                std::move($1), "=", ddbsh::Value::null_value());
        }
    | lee_lhs K_IS K_NOT K_NULL
        {
            // IS NOT NULL desugars to `NOT (= <null>) AND attribute_exists(a)`
            // (arch §6, §14 item 4). The lhs path is used by both leaves.
            auto cmp = std::make_unique<ddbsh::engine::LogicalComparison>(
                $1, "=", ddbsh::Value::null_value());
            cmp->negate();
            auto exists = std::make_unique<ddbsh::engine::LogicalAttributeExists>(
                std::move($1), true);
            $$ = ddbsh::engine::LogicalAnd::make_and(std::move(cmp), std::move(exists));
        }
    | lee_lhs K_BETWEEN lee_rhs K_AND lee_rhs
        {
            $$ = std::make_unique<ddbsh::engine::LogicalBetween>(
                std::move($1), std::move($3), std::move($5));
        }
    | lee_lhs K_IN '(' lee_rhs_list ')'
        {
            $$ = std::make_unique<ddbsh::engine::LogicalIn>(std::move($1), std::move($4));
        }
    | K_ATTRIBUTE_EXISTS '(' lee_lhs ')'
        {
            $$ = std::make_unique<ddbsh::engine::LogicalAttributeExists>(std::move($3), true);
        }
    | K_ATTRIBUTE_NOT_EXISTS '(' lee_lhs ')'
        {
            // attribute_not_exists = negated attribute_exists (arch §14 item 4).
            $$ = std::make_unique<ddbsh::engine::LogicalAttributeExists>(std::move($3), false);
        }
    | K_ATTRIBUTE_TYPE '(' lee_lhs ',' attribute_type ')'
        {
            // Carry the scalar-type token ("N"/"S"/"B") as a string, decoupling
            // the expression tree from the AST scalar-type enum.
            std::string type_token = $5 == ddbsh::engine::ScalarType::n   ? "N"
                                     : $5 == ddbsh::engine::ScalarType::s ? "S"
                                                                          : "B";
            $$ = std::make_unique<ddbsh::engine::LogicalAttributeType>(
                std::move($3), std::move(type_token));
        }
    | K_BEGINS_WITH '(' lee_lhs ',' string ')'
        {
            $$ = std::make_unique<ddbsh::engine::LogicalBeginsWith>(
                std::move($3), ddbsh::Value::string_value(std::move($5)));
        }
    | K_CONTAINS '(' lee_lhs ',' string ')'
        {
            $$ = std::make_unique<ddbsh::engine::LogicalContains>(
                std::move($3), ddbsh::Value::string_value(std::move($5)));
        }
    | K_SIZE '(' lee_lhs ')' comparison_op lee_rhs
        {
            // The lhs becomes the literal path `size(<attr>)` (arch §6, §14).
            std::string lhs = "size(" + $3 + ")";
            $$ = std::make_unique<ddbsh::engine::LogicalComparison>(
                std::move(lhs), $5, std::move($6));
        }
    ;

/* lee_lhs is an attribute path: a bare name, a `[subscript]` index, or a dotted
 * sub-path. Built as a plain string (the symbol table tokenizes it per segment
 * at serialization time, arch §6). */
lee_lhs:
      string                           { $$ = std::move($1); }
    | lee_lhs '[' T_WHOLE_NUMBER ']'   { $$ = $1 + "[" + $3 + "]"; }
    | lee_lhs '.' lee_lhs              { $$ = $1 + "." + $3; }
    ;

comparison_op:
      T_EQUAL                   { $$ = std::move($1); }
    | T_GREATER_THAN            { $$ = std::move($1); }
    | T_GREATER_THAN_OR_EQUAL   { $$ = std::move($1); }
    | T_LESS_THAN               { $$ = std::move($1); }
    | T_LESS_THAN_OR_EQUAL      { $$ = std::move($1); }
    | T_NOT_EQUAL               { $$ = std::move($1); }
    ;

optional_return_consumed_capacity:
      %empty
        { $$ = ddbsh::engine::ReturnConsumedCapacity::none; }
    | K_RETURN K_CONSUMED K_CAPACITY K_INDEXES
        { $$ = ddbsh::engine::ReturnConsumedCapacity::indexes; }
    | K_RETURN K_CONSUMED K_CAPACITY K_TOTAL
        { $$ = ddbsh::engine::ReturnConsumedCapacity::total; }
    ;

/* A string literal source: double-quoted (unquoted here, outer quotes stripped
 * exactly as the original unquote() does — "" doubling is NOT collapsed, arch
 * §4) or a bare unquoted identifier. */
string:
      T_DQUOTED_STRING   { $$ = ddbsh::engine::unquote($1); }
    | T_UNQUOTED_STRING  { $$ = std::move($1); }
    ;

region_name:
      string             { $$ = std::move($1); }
    ;

brief_clause:
      %empty             { $$ = false; }
    | K_BRIEF            { $$ = true; }
    ;

like_clause:
      %empty             { $$ = std::string{}; }
    | K_LIKE string      { $$ = std::move($2); }
    ;

if_exists_clause:
      %empty             { $$ = false; }
    | K_IF K_EXISTS      { $$ = true; }
    ;

if_not_exists_clause:
      %empty             { $$ = false; }
    | K_IF K_NOT K_EXISTS { $$ = true; }
    ;

nowait_clause:
      %empty             { $$ = false; }
    | K_NOWAIT           { $$ = true; }
    ;

%%

void ddbsh::engine::Parser::error(const std::string& msg) {
    // Compose the byte-exact user string "[line, col] syntax error at <lexeme>"
    // (arch §9, tenet 5). The bison message is kept verbose only so the parser
    // generates symbol_name() (used by the token-stream debug seam); the
    // user-facing base string the original emitted is simply "syntax error", so
    // we discard the verbose text and pass the fidelity string.
    (void)msg;
    drv.set_error("syntax error");
}
