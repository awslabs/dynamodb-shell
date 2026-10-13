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

%type <std::string> string region_name connect_endpoint like_clause
%type <bool> brief_clause if_exists_clause nowait_clause

%%

/* One `;`-terminated statement per parse (the engine parses one command at a
 * time; the original's yyparse likewise YYACCEPTs after each command). The
 * EXPLAIN prefix wraps a ddl/dml command (arch §7); it sets the AST's explain
 * flag rather than mutating a shared command object (P2a-02 §4.2). On a syntax
 * error, bison records the Error via error() and recovers to the next ';'; the
 * Driver then reports failure. */
input:
      %empty
        { drv.set_empty_result(); }
    | SEMICOLON
        { drv.set_empty_result(); }
    | command SEMICOLON
        { drv.set_result(std::move($1), false); }
    | K_EXPLAIN explainable SEMICOLON
        { drv.set_result(std::move($2), true); }
    | error SEMICOLON
        { /* error already recorded by error(); recover to ';' */ }
    ;

command:
      control_command    { $$ = std::move($1); }
    | ddl_command        { $$ = std::move($1); }
    | show_command       { $$ = std::move($1); }
    ;

/* EXPLAIN accepts ddl/dml commands only (as the original did); SHOW/CONNECT/
 * QUIT are not explainable. dml (SELECT/INSERT) joins this list when it lands. */
explainable:
      ddl_command        { $$ = std::move($1); }
    ;

control_command:
      connect_command    { $$ = std::move($1); }
    | quit_command       { $$ = std::move($1); }
    ;

ddl_command:
      drop_table_command { $$ = std::move($1); }
    | describe_command   { $$ = std::move($1); }
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
