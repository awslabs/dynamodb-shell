// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// lex_debug.hpp — engine-internal token-stream helper for tokenizer tests.
//
// PRIVATE engine header (not installed). It exposes a thin way to run the
// reentrant scanner over a string and recover the exact token sequence, so the
// §4 tokenizer quirks can be unit-tested directly without the full grammar.
// This is a test/diagnostic seam only; it is NOT part of the public API and
// carries no terminal or SDK dependency.

#ifndef DDBSH_SRC_PARSER_LEX_DEBUG_HPP
#define DDBSH_SRC_PARSER_LEX_DEBUG_HPP

#include <string>
#include <string_view>
#include <vector>

namespace ddbsh::engine {

/// One scanned token: its bison symbol name (e.g. "T_WHOLE_NUMBER",
/// "K_SELECT") and the verbatim matched lexeme.
struct LexToken {
    std::string name;
    std::string text;
};

/// Run the scanner over `input` and return the token sequence, excluding the
/// terminating end-of-input marker. Intended for tests/diagnostics only.
std::vector<LexToken> debug_lex(std::string_view input);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PARSER_LEX_DEBUG_HPP
