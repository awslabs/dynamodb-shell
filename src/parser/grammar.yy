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
    #include <optional>
    #include <string>
    #include <utility>
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

/* ---- Vector search (phase 4b) -------------------------------------------- */
%token K_COSINE
%token K_DIMENSIONS
%token K_DISTANCE
%token K_DOT_PRODUCT
%token K_EUCLIDEAN
%token K_FILTER
%token K_NEAREST
%token K_SEARCH
%token K_VECTOR
%token K_VSI

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
%type <ddbsh::engine::CommandNode> create_backup_command drop_backup_command
%type <ddbsh::engine::CommandNode> describe_backup_command restore_command
%type <ddbsh::engine::CommandNode> show_backups_command show_limits_command
%type <ddbsh::engine::CommandNode> show_create_table_command
%type <std::string> optional_backup_type optional_table_name
%type <ddbsh::engine::CommandNode> create_table_command
%type <ddbsh::engine::CommandNode> alter_table_command
%type <ddbsh::engine::CommandNode> dml_command write_command
%type <ddbsh::engine::CommandNode> insert_command replace_command
%type <ddbsh::engine::CommandNode> update_command upsert_command delete_command
%type <ddbsh::engine::CommandNode> help_command
%type <ddbsh::engine::CommandNode> transact_command exists_command
%type <ddbsh::engine::CommandNode> transact_get_command transact_write_command

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
%type <ddbsh::engine::KeySchema> key_schema primary_key key_role_list
%type <ddbsh::engine::KeyRole> key_role
%type <std::vector<std::string>> key_attr_group key_attr_name_list
%type <ddbsh::engine::BillingModeAndThroughput> billing_mode_and_throughput
%type <std::optional<ddbsh::engine::BillingModeAndThroughput>> optional_billing_mode_and_throughput
%type <ddbsh::engine::WarmThroughput> warm_throughput
%type <std::optional<ddbsh::engine::WarmThroughput>> optional_warm_throughput
%type <std::vector<ddbsh::engine::Gsi>> gsi_list gsi_specification
%type <ddbsh::engine::Gsi> gsi
%type <ddbsh::engine::GsiUpdate> alter_table_gsi_create alter_table_gsi_drop
%type <ddbsh::engine::GsiUpdate> alter_table_gsi_update_element
%type <std::vector<ddbsh::engine::GsiUpdate>> alter_table_gsi_update
%type <ddbsh::engine::ProvisionedThroughputOverride> provisioned_throughput_override
%type <std::optional<ddbsh::engine::ProvisionedThroughputOverride>> optional_provisioned_throughput_override
%type <ddbsh::engine::ReplicaGsi> replica_gsi_specification_list_entry
%type <std::vector<ddbsh::engine::ReplicaGsi>> replica_gsi_specification_list optional_replica_gsi_specification
%type <std::vector<ddbsh::engine::Lsi>> lsi_list lsi_specification
%type <ddbsh::engine::Lsi> lsi
%type <ddbsh::engine::Projection> index_projection
%type <std::vector<std::string>> index_projection_list

/* ---- Vector search (phase 4b) -------------------------------------------- */
%type <std::vector<ddbsh::engine::VectorIndexDef>> vector_index_list vector_index_specification
%type <ddbsh::engine::VectorIndexDef> vector_index
%type <ddbsh::engine::VectorDistance> vector_distance_function
%type <std::vector<ddbsh::engine::SearchSchemaElem>> optional_vector_search_schema vector_search_schema_list
%type <ddbsh::engine::SearchSchemaElem> vector_search_schema_element
%type <std::pair<std::optional<ddbsh::Value>, std::optional<int>>> optional_vector_search_clause
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

/* ---- UPDATE / UPSERT / DELETE SET-REMOVE value types (W5 / 2c) -----------
 * The SET clause builds a vector of engine::UpdateSetElement; REMOVE a vector
 * of attribute paths; use_lhs is the attribute-copy operand (a plain name).
 * Values are MOVED up the productions (no NEW/FREE). */
%type <ddbsh::engine::UpdateSetElement> set_element use_lhs
%type <std::vector<ddbsh::engine::UpdateSetElement>> set_list
%type <std::vector<std::string>> remove_list

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
    | help_command       { $$ = std::move($1); }
    | transact_command   { $$ = std::move($1); }
    ;

/* EXPLAIN accepts ddl/dml commands and transactions (as the original did);
 * SHOW/CONNECT/QUIT are not explainable. dml is the write family
 * (INSERT/REPLACE/UPDATE/UPSERT/DELETE/EXISTS) and the read family (SELECT via
 * get_command). A transaction EXPLAIN serializes the ONE batch request
 * (TransactGetItems / TransactWriteItems) and runs no enumeration (D-2d-12). */
explainable:
      ddl_command        { $$ = std::move($1); }
    | dml_command        { $$ = std::move($1); }
    | transact_command   { $$ = std::move($1); }
    ;

control_command:
      connect_command    { $$ = std::move($1); }
    | quit_command       { $$ = std::move($1); }
    ;

ddl_command:
      drop_table_command { $$ = std::move($1); }
    | describe_command   { $$ = std::move($1); }
    | create_table_command { $$ = std::move($1); }
    | alter_table_command  { $$ = std::move($1); }
    | create_backup_command   { $$ = std::move($1); }
    | drop_backup_command     { $$ = std::move($1); }
    | describe_backup_command { $$ = std::move($1); }
    | restore_command         { $$ = std::move($1); }
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
    | update_command     { $$ = std::move($1); }
    | upsert_command     { $$ = std::move($1); }
    | delete_command     { $$ = std::move($1); }
    | exists_command     { $$ = std::move($1); }
    ;

show_command:
      show_tables_command   { $$ = std::move($1); }
    | show_version_command  { $$ = std::move($1); }
    | show_backups_command      { $$ = std::move($1); }
    | show_limits_command       { $$ = std::move($1); }
    | show_create_table_command { $$ = std::move($1); }
    ;

quit_command:
      K_QUIT
        { $$ = ddbsh::engine::QuitCmd{}; }
    ;

show_version_command:
      K_SHOW K_VERSION
        { $$ = ddbsh::engine::ShowVersionCmd{}; }
    ;

/* ---- HELP (D-B) ----------------------------------------------------------
 * A faithful port of the original parser.y `help_command` productions. The
 * original printed a fixed usage string from each production DURING the parse
 * and yielded no command (arch §5); to keep the engine terminal-free we parse
 * HELP to a HelpCmd carrying the topic and emit the usage text from execute()
 * as a ReportOutcome (the same move SHOW VERSION made). The usage strings live
 * in src/parser/help_text.cpp, byte-for-byte faithful so `help ...;` matches
 * `ddbsh-old`. The trailing ';' is consumed by `input` (as for every command
 * here), so these forms omit it. All referenced keyword tokens already exist
 * in the token set (arch §4); the help text describes command syntax and does
 * not require the command itself to be implemented. */
help_command:
      K_HELP
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::general}; }
    | K_HELP K_EXPLAIN
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::explain}; }
    | K_HELP K_BEGIN
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::begin}; }
    | K_HELP K_COMMIT
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::commit}; }
    | K_HELP K_ROLLBACK
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::rollback}; }
    | K_HELP K_CONNECT
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::connect}; }
    | K_HELP K_DROP
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::drop}; }
    | K_HELP K_DROP K_TABLE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::drop_table}; }
    | K_HELP K_DROP K_BACKUP
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::drop_backup}; }
    | K_HELP K_DESCRIBE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::describe}; }
    | K_HELP K_DESCRIBE K_BACKUP
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::describe_backup}; }
    | K_HELP K_SHOW
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::show}; }
    | K_HELP K_SHOW K_TABLES
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::show_tables}; }
    | K_HELP K_SHOW K_LIMITS
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::show_limits}; }
    | K_HELP K_SHOW K_BACKUPS
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::show_backups}; }
    | K_HELP K_SHOW K_VERSION
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::show_version}; }
    | K_HELP K_SHOW K_CREATE K_TABLE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::show_create_table}; }
    | K_HELP K_CREATE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::create}; }
    | K_HELP K_CREATE K_TABLE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::create_table}; }
    | K_HELP K_ALTER K_TABLE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::alter_table}; }
    | K_HELP K_BACKUP
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::backup}; }
    | K_HELP K_RESTORE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::restore}; }
    | K_HELP K_SELECT
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::select_statement}; }
    | K_HELP K_INSERT
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::insert}; }
    | K_HELP K_REPLACE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::replace}; }
    | K_HELP K_UPDATE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::update}; }
    | K_HELP K_UPSERT
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::upsert}; }
    | K_HELP K_DELETE
        { $$ = ddbsh::engine::HelpCmd{ddbsh::engine::HelpTopic::del}; }
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

/* ---- BACKUP / RESTORE / SHOW (2f) ----------------------------------------
 * Faithful ports of the original create_backup/drop_backup/describe_backup/
 * restore_backup/restore_pitr/show_backups/show_limits/show_create_table
 * productions (parser.y). The trailing ';' is consumed by `input`, so each form
 * omits it. All referenced tokens already exist (added as HELP topics earlier).
 * N1 (RESTORE slot order): in BOTH restore forms the string after K_TABLE is
 * the TARGET (new table); the string after K_FROM is the SOURCE (backup ARN, or
 * source table for PITR). N2: ARNs/timestamps are double-quoted at the prompt;
 * the PITR timestamp is a T_TIMESTAMP literal, unquoted here. */
create_backup_command:
      K_BACKUP K_TABLE string K_CALLED string
        {
            ddbsh::engine::CreateBackupCmd cmd;
            cmd.table = std::move($3);
            cmd.backup_name = std::move($5);
            $$ = std::move(cmd);
        }
    ;

drop_backup_command:
      K_DROP K_BACKUP string
        { $$ = ddbsh::engine::DropBackupCmd{std::move($3)}; }
    ;

describe_backup_command:
      K_DESCRIBE K_BACKUP string
        { $$ = ddbsh::engine::DescribeBackupCmd{std::move($3)}; }
    ;

restore_command:
      K_RESTORE K_TABLE string K_FROM K_BACKUP string
        {
            ddbsh::engine::RestoreCmd cmd;
            cmd.kind = ddbsh::engine::RestoreCmd::Kind::from_backup;
            cmd.target = std::move($3);
            cmd.source = std::move($6);
            $$ = std::move(cmd);
        }
    | K_RESTORE K_TABLE string K_FROM string K_TO K_PITR T_TIMESTAMP
        {
            ddbsh::engine::RestoreCmd cmd;
            cmd.kind = ddbsh::engine::RestoreCmd::Kind::to_pitr;
            cmd.target = std::move($3);
            cmd.source = std::move($5);
            cmd.timestamp = ddbsh::engine::unquote($8);
            $$ = std::move(cmd);
        }
    ;

show_backups_command:
      K_SHOW optional_backup_type K_BACKUPS optional_table_name
        {
            ddbsh::engine::ShowBackupsCmd cmd;
            cmd.backup_type = std::move($2);
            cmd.table = std::move($4);
            $$ = std::move(cmd);
        }
    ;

optional_backup_type:
      %empty    { $$ = std::string{}; }
    | string    { $$ = std::move($1); }
    ;

optional_table_name:
      %empty    { $$ = std::string{}; }
    | string    { $$ = std::move($1); }
    ;

show_limits_command:
      K_SHOW K_LIMITS
        { $$ = ddbsh::engine::ShowLimitsCmd{}; }
    ;

show_create_table_command:
      K_SHOW K_CREATE K_TABLE if_not_exists_clause nowait_clause string
        {
            ddbsh::engine::ShowCreateTableCmd cmd;
            cmd.if_not_exists = $4;
            cmd.nowait = $5;
            cmd.table = std::move($6);
            $$ = std::move(cmd);
        }
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
      gsi_list lsi_list vector_index_list optional_stream_specification optional_sse_specification
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
            cmd.vector_indexes = std::move($14);
            cmd.stream = std::move($15);
            cmd.sse = std::move($16);
            cmd.table_class = $17;
            cmd.deletion_protection = $18;
            cmd.tags = std::move($19);
            $$ = std::move(cmd);
        }
    ;

/* ---- ALTER TABLE (2e-1 SET non-GSI + 2e-2 GSI -> UpdateTable) ------------
 * Faithful port of the original alter_table_command SET / GSI forms that map
 * to one CUpdateTableCommand -> UpdateTable (parser.y). The trailing ';' is
 * consumed by `input`, so each form omits it. Each alternative builds ONE
 * AlterTableCmd and fills only the field(s) the clause provided (D-2e-2); the
 * executor copies them into an UpdateTableIn and the adapter sets only the
 * present fields on the UpdateTableRequest, matching the original's "leave the
 * rest unset" behavior. The billing+warm form fills both `billing` and `warm`;
 * the standalone warm form fills only `warm` (arch §8, D-2e-3/-4).
 *
 * GSI forms (2e-2): CREATE GSI carries the new key attribute_definitions plus
 * one create GsiUpdate; DROP GSI carries one drop GsiUpdate; UPDATE GSI carries
 * a list of update GsiUpdates. The combined `SET billing ... UPDATE GSI` form
 * applies the D-2e-6 on-demand gate in its action: when the table billing mode
 * is on-demand the GSI updates are dropped (the index throughput is implied)
 * and a flag records the (unpinned) informational diagnostic. SET TTL (2e-3)
 * builds an AlterTtlCmd (DescribeTimeToLive + UpdateTimeToLive); SET PITR (2e-4)
 * builds an AlterPitrCmd (UpdateContinuousBackups); the REPLICA forms land in
 * 2e-5.
 *
 * LALR(1): the first tokens after K_SET are all distinct (K_BILLING / K_WARM /
 * K_SSE / K_TABLE / K_STREAM / K_DELETION / K_TTL / K_PITR). The shared prefix
 * `K_SET billing_mode_and_throughput optional_warm_throughput` of the bare SET
 * form vs. the combined UPDATE-GSI form is resolved by one-token lookahead
 * (reduce on FOLLOW vs. shift K_UPDATE), exactly as the original factored it. */
alter_table_command:
      K_ALTER K_TABLE string K_SET billing_mode_and_throughput optional_warm_throughput
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.billing = std::move($5);
            cmd.warm = std::move($6);
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET warm_throughput
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.warm = std::move($5);
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET sse_specification
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.sse = std::move($5);
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET table_class
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.table_class = $5;
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET stream_specification
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.stream = std::move($5);
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET deletion_protection
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.deletion_protection = $5;
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string '(' attribute_name_type_list ')' K_CREATE K_GSI alter_table_gsi_create
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.attribute_definitions = std::move($5);
            cmd.gsi_updates.push_back(std::move($9));
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_DROP K_GSI alter_table_gsi_drop
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.gsi_updates.push_back(std::move($6));
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string '(' attribute_name_type_list ')' K_CREATE vsi_keyword vector_index
        {
            /* CREATE vsi|vector|vector index (phase 4b, D-4b-9). Mirrors CREATE
             * GSI: the new SEARCH KEY / PROJECTING INCLUDE key attributes ride
             * in attribute_definitions; one Create VectorIndexUpdate carries the
             * index. The 1.11.916 model has no in-place Update (D-4b-4). */
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.attribute_definitions = std::move($5);
            ddbsh::engine::VectorIndexUpdateDef u;
            u.create = std::move($9);
            cmd.vector_index_updates.push_back(std::move(u));
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_DROP vsi_keyword string
        {
            /* DROP vsi|vector|vector index <name> (phase 4b). One Delete
             * VectorIndexUpdate carries the index name. */
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            ddbsh::engine::VectorIndexUpdateDef u;
            u.drop = std::move($6);
            cmd.vector_index_updates.push_back(std::move(u));
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET billing_mode_and_throughput optional_warm_throughput
            K_UPDATE K_GSI '(' alter_table_gsi_update ')'
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            // D-2e-6: setting the table to on-demand implies the index
            // throughput; the original drops the GSI update list and logs an
            // informational message. Keep the gate; carry the GSI updates only
            // when the table billing mode is PROVISIONED.
            const bool provisioned = $5.mode == ddbsh::engine::BillingMode::provisioned;
            cmd.billing = std::move($5);
            cmd.warm = std::move($6);
            if (provisioned) {
                cmd.gsi_updates = std::move($10);
            } else {
                cmd.ignored_gsi_on_demand = true;
            }
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_UPDATE K_GSI '(' alter_table_gsi_update ')'
        {
            ddbsh::engine::AlterTableCmd cmd;
            cmd.table = std::move($3);
            cmd.gsi_updates = std::move($7);
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET K_TTL K_DISABLED
        {
            /* SET TTL DISABLED: builds an AlterTtlCmd (DescribeTTL +
             * UpdateTimeToLive, D-2e-7), a distinct ALTER node sharing the one
             * StatementKind::alter_table. No attribute on disable — the executor
             * reuses the existing TTL attribute name from the probe. */
            ddbsh::engine::AlterTtlCmd cmd;
            cmd.table = std::move($3);
            cmd.enabled = false;
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET K_TTL '(' string ')'
        {
            /* SET TTL ( attr ): enable TTL on the named attribute. */
            ddbsh::engine::AlterTtlCmd cmd;
            cmd.table = std::move($3);
            cmd.enabled = true;
            cmd.attribute = std::move($7);
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET K_PITR K_ENABLED
        {
            /* SET PITR ENABLED: builds an AlterPitrCmd (UpdateContinuousBackups,
             * D-2e-8), a distinct ALTER node sharing the one
             * StatementKind::alter_table. K_PITR is a distinct first token after
             * K_SET, so no new LALR conflict. */
            ddbsh::engine::AlterPitrCmd cmd;
            cmd.table = std::move($3);
            cmd.enabled = true;
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_SET K_PITR K_DISABLED
        {
            /* SET PITR DISABLED. */
            ddbsh::engine::AlterPitrCmd cmd;
            cmd.table = std::move($3);
            cmd.enabled = false;
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_DROP K_REPLICA string
        {
            /* DROP REPLICA region: builds an AlterReplicaCmd (UpdateTable with a
             * DeleteReplicationGroupMemberAction, D-2e-9). K_REPLICA is a
             * distinct token after K_DROP (vs K_GSI), so no new LALR conflict. */
            ddbsh::engine::AlterReplicaCmd cmd;
            cmd.table = std::move($3);
            cmd.replica.action = ddbsh::engine::ReplicaUpdate::Action::drop;
            cmd.replica.region = std::move($6);
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_ADD K_REPLICA string optional_table_class
            optional_provisioned_throughput_override optional_replica_gsi_specification
        {
            /* ADD REPLICA region [table class] [( N rcu )] [GSI (...)]: builds an
             * AlterReplicaCmd (UpdateTable with a CreateReplicationGroupMemberAction).
             * optional_table_class defaults to STANDARD when absent, which the
             * adapter always sets as the TableClassOverride (matching the
             * original's always-SetTableClassOverride behavior). */
            ddbsh::engine::AlterReplicaCmd cmd;
            cmd.table = std::move($3);
            cmd.replica.action = ddbsh::engine::ReplicaUpdate::Action::add;
            cmd.replica.region = std::move($6);
            cmd.replica.table_class = $7;
            cmd.replica.throughput_override = std::move($8);
            cmd.replica.replica_gsis = std::move($9);
            $$ = std::move(cmd);
        }
    | K_ALTER K_TABLE string K_UPDATE K_REPLICA string optional_table_class
            optional_provisioned_throughput_override optional_replica_gsi_specification
        {
            /* UPDATE REPLICA region ...: builds an AlterReplicaCmd (UpdateTable
             * with an UpdateReplicationGroupMemberAction). Same fields as ADD;
             * K_REPLICA is a distinct token after K_UPDATE (vs K_GSI). */
            ddbsh::engine::AlterReplicaCmd cmd;
            cmd.table = std::move($3);
            cmd.replica.action = ddbsh::engine::ReplicaUpdate::Action::update;
            cmd.replica.region = std::move($6);
            cmd.replica.table_class = $7;
            cmd.replica.throughput_override = std::move($8);
            cmd.replica.replica_gsis = std::move($9);
            $$ = std::move(cmd);
        }
    ;

/* ---- GSI update actions (2e-2) -------------------------------------------
 * Each produces one engine::GsiUpdate (the SDK-free carrier for a
 * GlobalSecondaryIndexUpdate). CREATE reuses the CREATE-TABLE key_schema /
 * index_projection / billing / warm non-terminals verbatim. An UPDATE element
 * sets EITHER billing OR warm (the original's two update-element forms), never
 * both in one element. DROP carries only the index name. */
alter_table_gsi_create:
      string K_ON key_schema index_projection optional_billing_mode_and_throughput optional_warm_throughput
        {
            ddbsh::engine::GsiUpdate g;
            g.action = ddbsh::engine::GsiUpdate::Action::create;
            g.index_name = std::move($1);
            g.key_schema = std::move($3);
            g.projection = std::move($4);
            g.billing = std::move($5);
            g.warm = std::move($6);
            $$ = std::move(g);
        }
    ;

alter_table_gsi_drop:
      string
        {
            ddbsh::engine::GsiUpdate g;
            g.action = ddbsh::engine::GsiUpdate::Action::drop;
            g.index_name = std::move($1);
            $$ = std::move(g);
        }
    ;

alter_table_gsi_update_element:
      string K_SET billing_mode_and_throughput
        {
            ddbsh::engine::GsiUpdate g;
            g.action = ddbsh::engine::GsiUpdate::Action::update;
            g.index_name = std::move($1);
            g.billing = std::move($3);
            $$ = std::move(g);
        }
    | string K_SET warm_throughput
        {
            ddbsh::engine::GsiUpdate g;
            g.action = ddbsh::engine::GsiUpdate::Action::update;
            g.index_name = std::move($1);
            g.warm = std::move($3);
            $$ = std::move(g);
        }
    ;

alter_table_gsi_update:
      alter_table_gsi_update_element
        {
            $$ = std::vector<ddbsh::engine::GsiUpdate>{};
            $$.push_back(std::move($1));
        }
    | alter_table_gsi_update ',' alter_table_gsi_update_element
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

/* ---- REPLICA support non-terminals (2e-5) --------------------------------
 * Port of the original's provisioned_throughput_override /
 * optional_provisioned_throughput_override / optional_replica_gsi_specification
 * / replica_gsi_specification_list(_entry) (parser.y). A replica throughput
 * override carries READ capacity only (`( N rcu )` — the original set only
 * RCU). The per-replica GSI list is `GSI ( name ( N rcu ), ... )`.
 * optional_table_class is reused from CREATE TABLE (it defaults to STANDARD
 * when absent, which the adapter always sets as the TableClassOverride). */
provisioned_throughput_override:
      '(' T_WHOLE_NUMBER K_RCU ')'
        { $$ = ddbsh::engine::ProvisionedThroughputOverride{ddbsh::engine::parse_ll($2)}; }
    ;

optional_provisioned_throughput_override:
      provisioned_throughput_override  { $$ = std::move($1); }
    | %empty                           { $$ = std::nullopt; }
    ;

optional_replica_gsi_specification:
      K_GSI '(' replica_gsi_specification_list ')'  { $$ = std::move($3); }
    | %empty                                        { $$ = std::vector<ddbsh::engine::ReplicaGsi>{}; }
    ;

replica_gsi_specification_list:
      replica_gsi_specification_list_entry
        {
            $$ = std::vector<ddbsh::engine::ReplicaGsi>{};
            $$.push_back(std::move($1));
        }
    | replica_gsi_specification_list ',' replica_gsi_specification_list_entry
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

replica_gsi_specification_list_entry:
      string provisioned_throughput_override
        { $$ = ddbsh::engine::ReplicaGsi{std::move($1), std::move($2)}; }
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

/* A key role (HASH or RANGE) whose attribute list has 2+ members is written as
 * a parenthesized, comma-separated group followed by the role word; a
 * single-attribute key stays bare (today's form, unchanged). Roles may be
 * written in any order (HASH-before-RANGE, or the legacy RANGE-before-HASH):
 * the semantic action buckets each role into the KeySchema by role word
 * (add_key_role), so the canonical [HASH…, RANGE…] emission order is
 * reconstructed. ONE rule serves base-table primary keys, GSIs and LSIs; the
 * GSI-only restriction on composite keys is enforced semantically by the
 * executor's validation (validate_key_schemas in
 * src/execute/create_table_executor.cpp), not the grammar, so it yields
 * a clear error rather than a bare `syntax error` (phase 4a). */
key_schema:
      '(' key_role_list ')'  { $$ = std::move($2); }
    ;

key_role_list:
      key_role
        {
            ddbsh::engine::KeySchema ks;
            ddbsh::add_key_role(ks, $1.is_hash, std::move($1.attrs));
            $$ = std::move(ks);
        }
    | key_role_list ',' key_role
        {
            ddbsh::add_key_role($1, $3.is_hash, std::move($3.attrs));
            $$ = std::move($1);
        }
    ;

key_role:
      key_attr_group K_HASH   { $$ = ddbsh::engine::KeyRole{true, std::move($1)}; }
    | key_attr_group K_RANGE  { $$ = ddbsh::engine::KeyRole{false, std::move($1)}; }
    ;

key_attr_group:
      string                      { $$ = std::vector<std::string>{std::move($1)}; }
    | '(' key_attr_name_list ')'  { $$ = std::move($2); }
    ;

key_attr_name_list:
      string                           { $$ = std::vector<std::string>{std::move($1)}; }
    | key_attr_name_list ',' string    { $1.push_back(std::move($3)); $$ = std::move($1); }
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

/* ---- Vector index (phase 4b) --------------------------------------------
 * An optional trailing CREATE-TABLE clause that mirrors gsi_list/lsi_list. The
 * clause lead accepts three synonyms (D-4b-9): VSI (...) | VECTOR (...) |
 * VECTOR INDEX (...). Each index is `name ON vector-attr DIMENSIONS n DISTANCE
 * fn [SEARCH KEY (...)] PROJECTING ...`. The vector attribute is declared ONLY
 * here (ON <attr>), never in the top attribute-type list (D-4b-8): a vector is
 * an L-of-N, not a scalar key type. LALR(1)-safe: the clause lead tokens
 * (K_VSI / K_VECTOR) are distinct from every following clause's lead. */
vsi_keyword:
      K_VSI
    | K_VECTOR
    | K_VECTOR K_INDEX
    ;

vector_index_list:
      vsi_keyword '(' vector_index_specification ')'  { $$ = std::move($3); }
    | %empty  { $$ = std::vector<ddbsh::engine::VectorIndexDef>{}; }
    ;

vector_index_specification:
      vector_index
        {
            $$ = std::vector<ddbsh::engine::VectorIndexDef>{};
            $$.push_back(std::move($1));
        }
    | vector_index_specification ',' vector_index
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

vector_index:
      string K_ON string K_DIMENSIONS T_WHOLE_NUMBER K_DISTANCE vector_distance_function
      optional_vector_search_schema index_projection
        {
            ddbsh::engine::VectorIndexDef v;
            v.index_name = std::move($1);
            v.vector_attr = std::move($3);
            v.dimensions = ddbsh::engine::parse_ll($5);
            v.distance = $7;
            v.search_schema = std::move($8);
            v.projection = std::move($9);
            $$ = std::move(v);
        }
    ;

vector_distance_function:
      K_COSINE       { $$ = ddbsh::engine::VectorDistance::cosine; }
    | K_EUCLIDEAN    { $$ = ddbsh::engine::VectorDistance::euclidean; }
    | K_DOT_PRODUCT  { $$ = ddbsh::engine::VectorDistance::dot_product; }
    ;

optional_vector_search_schema:
      K_SEARCH K_KEY '(' vector_search_schema_list ')'  { $$ = std::move($4); }
    | %empty  { $$ = std::vector<ddbsh::engine::SearchSchemaElem>{}; }
    ;

vector_search_schema_list:
      vector_search_schema_element
        {
            $$ = std::vector<ddbsh::engine::SearchSchemaElem>{};
            $$.push_back(std::move($1));
        }
    | vector_search_schema_list ',' vector_search_schema_element
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

vector_search_schema_element:
      string K_HASH
        { $$ = ddbsh::engine::SearchSchemaElem{std::move($1),
                                               ddbsh::engine::SearchSchemaKind::hash}; }
    | string K_FILTER
        { $$ = ddbsh::engine::SearchSchemaElem{std::move($1),
                                               ddbsh::engine::SearchSchemaKind::inline_filter}; }
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

/* ---- UPDATE / UPSERT / DELETE (W5 / 2c) ----------------------------------
 * Faithful ports of the original update_command / upsert_command /
 * delete_command (parser.y). The trailing ';' is consumed by `input`, so each
 * form omits it. One UpdateCmd backs UPDATE and UPSERT (the `upsert` flag);
 * UPDATE has both SET and REMOVE forms, UPSERT only SET (original parity).
 * table_optional_index_name, optional_where_clause, optional_ratelimit, and
 * optional_return_value are reused from SELECT/INSERT. */
update_command:
      K_UPDATE table_optional_index_name K_SET set_list
      optional_where_clause optional_ratelimit optional_return_value
        {
            ddbsh::engine::UpdateCmd cmd;
            cmd.table = std::move($2.table);
            cmd.index = std::move($2.index);
            cmd.mutation = ddbsh::engine::Mutation::set;
            cmd.set = std::move($4);
            cmd.where = std::move($5);
            cmd.ratelimit = std::move($6);
            cmd.return_value = $7;
            cmd.upsert = false;
            $$ = std::move(cmd);
        }
    | K_UPDATE table_optional_index_name K_REMOVE remove_list
      optional_where_clause optional_ratelimit optional_return_value
        {
            ddbsh::engine::UpdateCmd cmd;
            cmd.table = std::move($2.table);
            cmd.index = std::move($2.index);
            cmd.mutation = ddbsh::engine::Mutation::remove;
            cmd.remove = std::move($4);
            cmd.where = std::move($5);
            cmd.ratelimit = std::move($6);
            cmd.return_value = $7;
            cmd.upsert = false;
            $$ = std::move(cmd);
        }
    ;

upsert_command:
      K_UPSERT table_optional_index_name K_SET set_list
      optional_where_clause optional_ratelimit optional_return_value
        {
            ddbsh::engine::UpdateCmd cmd;
            cmd.table = std::move($2.table);
            cmd.index = std::move($2.index);
            cmd.mutation = ddbsh::engine::Mutation::set;
            cmd.set = std::move($4);
            cmd.where = std::move($5);
            cmd.ratelimit = std::move($6);
            cmd.return_value = $7;
            cmd.upsert = true;
            $$ = std::move(cmd);
        }
    ;

delete_command:
      K_DELETE K_FROM table_optional_index_name
      optional_where_clause optional_ratelimit optional_return_value
        {
            ddbsh::engine::DeleteCmd cmd;
            cmd.table = std::move($3.table);
            cmd.index = std::move($3.index);
            cmd.where = std::move($4);
            cmd.ratelimit = std::move($5);
            cmd.return_value = $6;
            $$ = std::move(cmd);
        }
    ;

/* ---- Transactions (2d) ----------------------------------------------------
 * A whole `BEGIN; <inner statements> (COMMIT|ROLLBACK)` reduces to ONE
 * TransactCmd in a SINGLE parse (D-2d-1, the crux): BEGIN has no standalone
 * production, so the persistent scanner shifts past every inner ';' and
 * YYACCEPT fires only at the top-level `input: command [SEMICOLON]` after the
 * terminator. Unlike the original (whose select_command/write_command carried
 * their own trailing ';'), here each inner statement is followed by an explicit
 * SEMICOLON inside the transact_*_command lists, and the trailing ';' after
 * COMMIT/ROLLBACK is consumed by `input`. Homogeneity (all reads or all writes)
 * is enforced by the grammar: a mixed body has no production and is a syntax
 * error (preserve the original). */
transact_command:
      K_BEGIN SEMICOLON transact_get_command K_COMMIT
        {
            std::get<ddbsh::engine::TransactCmd>($3).rollback = false;
            $$ = std::move($3);
        }
    | K_BEGIN SEMICOLON transact_write_command K_COMMIT
        {
            std::get<ddbsh::engine::TransactCmd>($3).rollback = false;
            $$ = std::move($3);
        }
    | K_BEGIN SEMICOLON transact_get_command K_ROLLBACK
        {
            std::get<ddbsh::engine::TransactCmd>($3).rollback = true;
            $$ = std::move($3);
        }
    | K_BEGIN SEMICOLON transact_write_command K_ROLLBACK
        {
            std::get<ddbsh::engine::TransactCmd>($3).rollback = true;
            $$ = std::move($3);
        }
    | K_BEGIN SEMICOLON K_ROLLBACK
        {
            /* Empty transaction (itxt-1): renders nothing. No empty-COMMIT
             * production exists — `begin; commit;` stays a syntax error
             * (D-2d-7). `access` is irrelevant for an empty no-op. */
            ddbsh::engine::TransactCmd cmd;
            cmd.access = ddbsh::engine::TransactCmd::Access::read;
            cmd.rollback = true;
            $$ = std::move(cmd);
        }
    ;

transact_get_command:
      get_command SEMICOLON
        {
            ddbsh::engine::TransactCmd cmd;
            cmd.access = ddbsh::engine::TransactCmd::Access::read;
            cmd.items.push_back(std::move($1));
            $$ = std::move(cmd);
        }
    | transact_get_command get_command SEMICOLON
        {
            std::get<ddbsh::engine::TransactCmd>($1).items.push_back(std::move($2));
            $$ = std::move($1);
        }
    ;

transact_write_command:
      write_command SEMICOLON
        {
            ddbsh::engine::TransactCmd cmd;
            cmd.access = ddbsh::engine::TransactCmd::Access::write;
            cmd.items.push_back(std::move($1));
            $$ = std::move(cmd);
        }
    | transact_write_command write_command SEMICOLON
        {
            std::get<ddbsh::engine::TransactCmd>($1).items.push_back(std::move($2));
            $$ = std::move($1);
        }
    ;

/* EXISTS / NOT EXISTS <select> — a ConditionCheck inside a write transaction
 * (arch §7). The flag is set on the SelectCmd; standalone it is ignored and the
 * SELECT runs normally (D-2d-3). Guarded with get_if so the SELECT's own
 * semantic-error path (which yields EmptyCmd) never triggers bad_variant_access
 * (the engine never throws — D12). */
exists_command:
      K_EXISTS select_command
        {
            if (auto* s = std::get_if<ddbsh::engine::SelectCmd>(&$2)) {
                s->exists_mode = ddbsh::engine::ExistsMode::exists;
            }
            $$ = std::move($2);
        }
    | K_NOT K_EXISTS select_command
        {
            if (auto* s = std::get_if<ddbsh::engine::SelectCmd>(&$3)) {
                s->exists_mode = ddbsh::engine::ExistsMode::not_exists;
            }
            $$ = std::move($3);
        }
    ;

set_list:
      set_element
        {
            $$ = std::vector<ddbsh::engine::UpdateSetElement>{};
            $$.push_back(std::move($1));
        }
    | set_list ',' set_element
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

remove_list:
      lee_lhs
        {
            $$ = std::vector<std::string>{};
            $$.push_back(std::move($1));
        }
    | remove_list ',' lee_lhs
        { $1.push_back(std::move($3)); $$ = std::move($1); }
    ;

/* The four SET assignment forms (arch §6): plain value, attribute copy,
 * arithmetic (+/-), and if_not_exists(path,value). The if_not_exists form is
 * FIDELITY (not a divergence): it is in the original's keyword table and
 * grammar and the golden iut-ifne-* .exp require it (see UpdateSetElement in
 * ast.hpp — this corrects arch §12 #5 / D-2c-15). */
set_element:
      lee_lhs T_EQUAL lee_rhs
        {
            ddbsh::engine::UpdateSetElement e;
            e.lhs = std::move($1);
            e.rhs_value = std::move($3);
            $$ = std::move(e);
        }
    | lee_lhs T_EQUAL use_lhs '+' lee_rhs
        {
            $3.lhs = std::move($1);
            $3.op = "+";
            $3.rhs_value = std::move($5);
            $$ = std::move($3);
        }
    | lee_lhs T_EQUAL use_lhs '-' lee_rhs
        {
            $3.lhs = std::move($1);
            $3.op = "-";
            $3.rhs_value = std::move($5);
            $$ = std::move($3);
        }
    | lee_lhs T_EQUAL use_lhs
        {
            $3.lhs = std::move($1);
            $$ = std::move($3);
        }
    ;

/* use_lhs is the RHS operand that is NOT a plain value: either an
 * attribute-copy name (`#b`) or an `if_not_exists(#p, :v)` call. It builds a
 * partial UpdateSetElement (lhs filled in by set_element above), mirroring the
 * original's use_lhs production. */
use_lhs:
      T_UNQUOTED_STRING
        {
            ddbsh::engine::UpdateSetElement e;
            e.rhs_attribute = std::move($1);
            $$ = std::move(e);
        }
    | K_IF_NOT_EXISTS '(' lee_lhs ',' lee_rhs ')'
        {
            ddbsh::engine::UpdateSetElement e;
            e.if_not_exists_lhs = std::move($3);
            e.if_not_exists_value = std::move($5);
            $$ = std::move(e);
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
      optional_where_clause optional_vector_search_clause
      optional_return_consumed_capacity optional_ratelimit
        {
            if ($9.has_value() &&
                $8 != ddbsh::engine::ReturnConsumedCapacity::none) {
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
                cmd.search_vector = std::move($7.first);
                cmd.topk = $7.second;
                cmd.rcc = $8;
                cmd.ratelimit = std::move($9);
                $$ = std::move(cmd);
            }
        }
    ;

/* ---- Vector search tail (phase 4b, D-4b-1 rev) ---------------------------
 * `NEAREST <topk> TO [ numeric-list ]` — placed after the WHERE clause and
 * before RETURN CONSUMED CAPACITY / ratelimit. The whole number after NEAREST
 * is the required TopK; `TO <list>` (reusing lee_rhs's bracketed numeric list)
 * is the query vector. TopK and the vector are grammatically inseparable, so
 * "NEAREST without TopK" cannot be expressed. The planner (not the grammar)
 * requires the target index to be a vector index. Carried as a
 * (optional<Value>, optional<int>) pair so the empty alternative yields both
 * nullopt. */
optional_vector_search_clause:
      %empty
        { $$ = std::make_pair(std::optional<ddbsh::Value>{}, std::optional<int>{}); }
    | K_NEAREST T_WHOLE_NUMBER K_TO lee_rhs
        {
            $$ = std::make_pair(std::optional<ddbsh::Value>{std::move($4)},
                                std::optional<int>{static_cast<int>(
                                    ddbsh::engine::parse_ll($2))});
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
