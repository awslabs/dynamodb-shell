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

#include <algorithm>
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

Driver::Step Driver::parse_next() {
    // Fresh per-statement state; the scanner, EOF flag, and line/column cursor
    // persist across statements (the cursor stays continuous so a later
    // statement's error reports the original's `[line, col]`).
    result_.reset();
    error_.reset();

    Parser parser(*this);
    int rc = parser.parse();
    if (rc != 0 || error_.has_value()) {
        // A nonzero bison return, or an error recorded by error()/
        // set_semantic_error() (even when the grammar YYACCEPTed via the
        // `error SEMICOLON` recovery arm), is a failed statement.
        return Step::error;
    }
    // On success the grammar always set a result (a command or the empty
    // statement). A terminal empty parse — the empty statement reached because
    // the scanner is at end of input — ends the batch; a bare-';' empty
    // statement mid-batch (EOF not yet seen) is a real, no-op statement.
    if (!result_.has_value() || (result_->kind() == StatementKind::empty && saw_eof_)) {
        return Step::empty_at_eof;
    }
    return Step::statement;
}

bool Driver::parse() {
    Step step = parse_next();
    if (step == Step::error) {
        return false;
    }
    // Back-compat: Session::parse() treats a terminal empty parse (e.g. empty
    // or comment-only input) as the empty statement.
    if (step == Step::empty_at_eof && !result_.has_value()) {
        result_ = Statement{};
    }
    return true;
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
    // Follow it with a compiler-style caret diagnostic (P4a-02 item 3): the
    // offending source line, then a '^' pointing at the exact error column.
    // This is an additive diagnostic beyond the original's single error line;
    // no golden pins the syntax-error bytes (the suite matches "syntax error
    // at <tok>" as a substring regex), so parity is preserved.
    std::string caret = build_caret();
    if (!caret.empty()) {
        composed += "\n";
        composed += caret;
    }
    error_ = Error{Errc::parse_error, std::move(composed), std::nullopt, std::nullopt};
}

std::string Driver::build_caret() const {
    // Locate the start offset of the 1-based line_ within the borrowed input.
    std::size_t line_start = 0;
    int cur = 1;
    while (cur < line_ && line_start <= input_.size()) {
        std::size_t nl = input_.find('\n', line_start);
        if (nl == std::string_view::npos) {
            break;
        }
        line_start = nl + 1;
        ++cur;
    }
    if (line_start > input_.size()) {
        return {};
    }
    // Extract the offending physical line (without its trailing newline / CR).
    std::size_t nl = input_.find('\n', line_start);
    std::string_view src_line = input_.substr(
        line_start, nl == std::string_view::npos ? std::string_view::npos : nl - line_start);
    if (!src_line.empty() && src_line.back() == '\r') {
        src_line.remove_suffix(1);
    }
    if (src_line.empty()) {
        return {};
    }
    // Caret column (1-based): the first character of the offending lexeme, i.e.
    // the current cursor (one past the lexeme) backed up over the lexeme. An
    // empty lexeme (end of input) points the caret at the cursor itself.
    std::size_t col = static_cast<std::size_t>(column_);
    std::size_t lexeme_len = last_lexeme_.size();
    std::size_t caret_col = (col > lexeme_len) ? col - lexeme_len : col;
    if (caret_col < 1) {
        caret_col = 1;
    }
    // Indentation up to the caret: copy the source line's leading characters,
    // turning everything but tabs into spaces so a tab-rendered line still
    // lines up under a terminal.
    std::string indent;
    std::size_t limit = std::min<std::size_t>(caret_col - 1, src_line.size());
    indent.reserve(limit);
    for (std::size_t ix = 0; ix < limit; ++ix) {
        indent.push_back(src_line[ix] == '\t' ? '\t' : ' ');
    }
    std::string out;
    out.reserve(src_line.size() + indent.size() + 2);
    out.append(src_line);
    out.push_back('\n');
    out.append(indent);
    out.push_back('^');
    return out;
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
