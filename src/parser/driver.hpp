// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// driver.hpp — per-parse driver for the reentrant flex + bison C++ parser (D3).
//
// PRIVATE engine header (not installed): it references the generated bison
// header and the reentrant flex scanner handle, which must never reach
// include/ddbsh/ (D11). One Driver instance backs one Session::parse() call:
// it owns the flex scanner handle (via RAII init/destroy), carries the
// in-progress result and error state, and never outlives the parse (P2a-02
// §7.2). No global scanner/parser state exists, so multiple Sessions parse
// concurrently without interference.

#ifndef DDBSH_SRC_PARSER_DRIVER_HPP
#define DDBSH_SRC_PARSER_DRIVER_HPP

#include <optional>
#include <string>
#include <string_view>

#include "ddbsh/error.hpp"
#include "ddbsh/statement.hpp"

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

    void set_result(Statement s) { result_ = s; }
    const std::optional<Statement>& result() const { return result_; }

    /// Record a parse error with the byte-exact user message (tenet 5).
    void set_error(std::string message);
    const std::optional<Error>& error() const { return error_; }

private:
    std::string_view input_;
    std::size_t scan_offset_ = 0;  ///< byte cursor for the reentrant YY_INPUT
    void* scanner_ = nullptr;      ///< yyscan_t (opaque here; flex type in .cpp)
    std::optional<Statement> result_;
    std::optional<Error> error_;
};

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PARSER_DRIVER_HPP
