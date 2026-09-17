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

#include <string>

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

void Driver::set_error(std::string message) {
    error_ = Error{Errc::parse_error, std::move(message), std::nullopt, std::nullopt};
}

}  // namespace ddbsh::engine
