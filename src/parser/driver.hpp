// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// driver.hpp — per-parse driver for the reentrant flex + bison C++ parser (D3).
//
// PRIVATE engine header (not installed): it references the generated bison
// header and the reentrant flex scanner handle, which must never reach
// include/ddbsh/ (D11). One Driver instance backs one Session::parse() call:
// it owns the flex scanner handle (via RAII init/destroy), carries the
// in-progress result and error state, tracks scan position for byte-exact
// error locations, and never outlives the parse (P2a-02 §7.2). No global
// scanner/parser state exists, so multiple Sessions parse concurrently without
// interference.

#ifndef DDBSH_SRC_PARSER_DRIVER_HPP
#define DDBSH_SRC_PARSER_DRIVER_HPP

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "ddbsh/error.hpp"
#include "ddbsh/statement.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Per-parse state shared between the scanner, the generated parser, and the
/// grammar actions. Constructed and destroyed entirely within parse().
class Driver {
public:
    /// @param input  the whole command text to scan (borrowed for the parse).
    explicit Driver(std::string_view input);
    ~Driver();

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    /// Run scanner + parser over the input. On success `result` holds the
    /// parsed Statement; on failure `error` holds a parse_error.
    /// @returns true on success, false on parse/scan failure.
    bool parse();

    // ---- accessors used by the generated parser / grammar actions ----------
    void* scanner() { return scanner_; }
    std::string_view input() const { return input_; }
    std::size_t& scan_offset() { return scan_offset_; }

    /// Record the verbatim text of the most recently scanned lexeme and advance
    /// the line/column cursor over it (set by the scanner's YY_USER_ACTION).
    /// Mirrors the original lexer's count() semantics (arch §4): the cursor
    /// points just past the scanned token, so the error location matches the
    /// original's `[line, col]`.
    void on_token(const char* text, int length);
    const std::string& last_lexeme() const { return last_lexeme_; }

    // ---- result builders invoked by the grammar actions --------------------
    /// The empty statement (blank / comment-only / bare-';' input).
    void set_empty_result();
    /// Build the parse result from a command node and the EXPLAIN flag.
    void set_result(CommandNode command, bool explain);
    const std::optional<Statement>& result() const { return result_; }

    /// Move the parsed Statement out of the driver (Statement is move-only).
    /// Precondition: parse() returned true, so result_ holds a value.
    Statement release_result() { return std::move(result_.value()); }

    /// Record a parse error. `message` is the bison error text ("syntax
    /// error"); this composes the byte-exact user string `[line, col] message
    /// at <lexeme>` the original emitted (arch §9, tenet 5).
    void set_error(const std::string& message);

    /// Record a SEMANTIC error from a grammar action with a VERBATIM message
    /// (no `[line, col] ... at <lexeme>` framing). Used where the original
    /// emitted a plain logerror from inside a reduction — e.g. SELECT's
    /// "You cannot specify both a ratelimit and a return_consumed_capacity."
    /// The message is reproduced byte-for-byte (tenet 5); the recorded error
    /// makes parse() fail so the statement is rejected.
    void set_semantic_error(std::string message);
    const std::optional<Error>& error() const { return error_; }

private:
    std::string_view input_;
    std::size_t scan_offset_ = 0;  ///< byte cursor for the reentrant YY_INPUT
    void* scanner_ = nullptr;      ///< yyscan_t (opaque here; flex type in .cpp)
    std::string last_lexeme_;      ///< verbatim text of the last scanned lexeme
    int line_ = 1;                 ///< 1-based line of the scan cursor
    int column_ = 1;               ///< 1-based column of the scan cursor
    std::optional<Statement> result_;
    std::optional<Error> error_;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PARSER_DRIVER_HPP
