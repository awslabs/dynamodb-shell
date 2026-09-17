/* Copyright 2026 ddbsh reimplementation contributors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * grammar.yy — the engine grammar (bison C++ lalr1.cc skeleton, D3).
 *
 * 2a walking-skeleton scope: recognizes SHOW VERSION, QUIT/EXIT, and empty
 * input; every production moves an owning value up the stack (no NEW/FREE, no
 * raw owning pointers — P2a-02). The generated parser is a class
 * (ddbsh::engine::Parser) with a pure (reentrant) interface; its header stays
 * private behind Session::Impl (D11). The full grammar port lands in 2b and
 * preserves the tokenizer/keyword quirks in arch §4.
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
%token K_SHOW       "SHOW"
%token K_VERSION    "VERSION"
%token K_QUIT       "QUIT"

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
    // Byte-exact style deferred to 2b (arch s9 "[line,col] msg at tok" /
    // "syntax error at ;"); for the skeleton, record a parse_error carrying the
    // bison message so the failure path is observable and tested.
    drv.set_error(msg);
}
