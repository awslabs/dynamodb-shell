/* Copyright 2026 ddbsh reimplementation contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * grammar.yy — the engine grammar (bison C++ lalr1.cc skeleton, D3).
 *
 * Phase 2b, W1 (lexer port): this file now declares the COMPLETE token set the
 * ported tokenizer produces (arch §4), matching the original parser.y token
 * list one-for-one. The grammar PRODUCTIONS, however, still recognize only the
 * walking-skeleton command set (SHOW VERSION, QUIT, empty) — the full command
 * productions and the owning AST land in W2. Declaring every token now lets the
 * reentrant scanner (lexer.l) and its keyword table be ported in full and
 * unit-tested directly (see src/parser/lex_debug.*), while the build stays
 * green and the existing parse tests keep passing. Unused-token warnings from
 * bison are expected for this intermediate state and are not -Werror.
 *
 * The generated parser is a class (ddbsh::engine::Parser) with a reentrant
 * interface; its header stays private behind Session::Impl (D11). Token value
 * carriers use std::string (the original's char* strval + STRDUP), so there is
 * no manual free and ownership is clear (P2a-02).
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

%type <ddbsh::StatementKind> command

%%

input:
      %empty
        { drv.set_result(ddbsh::Statement{ddbsh::StatementKind::empty, false}); }
    | command SEMICOLON
        { drv.set_result(ddbsh::Statement{$1, false}); }
    ;

command:
      K_SHOW K_VERSION   { $$ = ddbsh::StatementKind::show_version; }
    | K_QUIT             { $$ = ddbsh::StatementKind::quit; }
    ;

%%

void ddbsh::engine::Parser::error(const std::string& msg) {
    // Byte-exact style deferred to W2 (arch §9 "[line,col] msg at tok" /
    // "syntax error at ;"); for now, record a parse_error carrying the bison
    // message so the failure path is observable and tested.
    drv.set_error(msg);
}
