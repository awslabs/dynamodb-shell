// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// driver.cpp — per-parse driver implementation (reentrant flex + bison).
//
// Owns the flex scanner handle for exactly one parse() call (RAII: yylex_init
// in the ctor path / yylex_destroy in the dtor), wires the Driver as yyextra so
// the reentrant YY_INPUT can pull from the borrowed input, then runs the
// generated Parser. No scanner/parser state survives the call (P2a-02 §7.2).

#include "parser/driver.hpp"

#include <memory>
#include <string>
#include <utility>

#include "parser/grammar.hpp"
#include "parser/lexer.hpp"

namespace ddbsh::engine {

Driver::Driver(std::string_view input) : input_(input) {
    yyscan_t sc = nullptr;
    ddbsh_yylex_init(&sc);
    ddbsh_yyset_extra(this, sc);
    scanner_ = sc;
}

Driver::~Driver() {
    if (scanner_ != nullptr) {
        ddbsh_yylex_destroy(static_cast<yyscan_t>(scanner_));
        scanner_ = nullptr;
    }
}

bool Driver::parse() {
    Parser parser(*this);
    int rc = parser.parse();
    // A nonzero bison return, or an error recorded by the error() hook, is a
    // failure. On success the grammar has set result_.
    return rc == 0 && !error_.has_value();
}

void Driver::on_token(const char* text, int length) {
    last_lexeme_.assign(text, static_cast<std::size_t>(length));
    // Advance the line/column cursor over the lexeme, matching the original
    // lexer's count() (arch §4): newline (or carriage return) resets the column
    // and bumps the line; any other char advances the column. After this the
    // cursor sits just past the token.
    for (int ix = 0; ix < length; ++ix) {
        if (text[ix] == '\n' || text[ix] == '\r') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
    }
}

void Driver::set_empty_result() {
    result_ = Statement{};
}

void Driver::set_result(CommandNode command, bool explain) {
    result_ =
        Statement{std::make_unique<ParsedStatement>(ParsedStatement{std::move(command), explain})};
}

void Driver::set_error(const std::string& message) {
    // Byte-exact user string: "[line, col] <message> at <lexeme>" — the format
    // the original yyerror emitted (parser.y: logerror("[%d, %d] %s at %s")).
    std::string composed = "[" + std::to_string(line_) + ", " + std::to_string(column_) + "] " +
                           message + " at " + last_lexeme_;
    error_ = Error{Errc::parse_error, std::move(composed), std::nullopt, std::nullopt};
}

void Driver::set_semantic_error(std::string message) {
    // A verbatim semantic error from a grammar reduction (no [line, col]
    // framing): the original printed these via a bare logerror(). Only record
    // the FIRST such error so an earlier diagnostic is not overwritten during
    // error recovery.
    if (!error_.has_value()) {
        error_ = Error{Errc::invalid_argument, std::move(message), std::nullopt, std::nullopt};
    }
}

}  // namespace ddbsh::engine
