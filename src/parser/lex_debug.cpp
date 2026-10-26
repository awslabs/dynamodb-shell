// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// lex_debug.cpp — token-stream helper implementation.
//
// Drives the reentrant scanner (via the ddbsh_lex bridge defined in the
// generated lexer) over a borrowed string, pairing each returned token's
// symbol name with the verbatim lexeme recorded on the Driver by the scanner's
// YY_USER_ACTION. Scanner setup/teardown is the Driver's RAII job (P2a-02 §7).

#include "parser/lex_debug.hpp"

#include "parser/driver.hpp"
#include "parser/grammar.hpp"

// The reentrant scanner bridge (defined in the generated lexer TU).
ddbsh::engine::Parser::symbol_type ddbsh_lex(ddbsh::engine::Driver& drv);

namespace ddbsh::engine {

std::vector<LexToken> debug_lex(std::string_view input) {
    Driver drv(input);
    std::vector<LexToken> tokens;
    for (;;) {
        Parser::symbol_type sym = ddbsh_lex(drv);
        if (sym.kind() == Parser::symbol_kind_type::S_YYEOF) {
            break;
        }
        tokens.push_back(LexToken{Parser::symbol_name(sym.kind()), drv.last_lexeme()});
    }
    return tokens;
}

}  // namespace ddbsh::engine
