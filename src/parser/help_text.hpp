// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// help_text.hpp — the HELP usage text, byte-for-byte faithful to the original.
//
// PRIVATE engine header (not installed). The original ddbsh printed a fixed
// usage string from each `help_command` grammar production during the parse
// (parser.y). This reimplementation keeps the engine terminal-free: HELP parses
// to a HelpCmd (src/parser/ast.hpp) carrying a HelpTopic, and execute() turns
// the topic's text into a ReportOutcome the Renderer emits. The strings here
// reproduce the original's printf output exactly so `help ...;` matches
// `ddbsh-old` byte-for-byte (tenet 1, D-B).

#ifndef DDBSH_SRC_PARSER_HELP_TEXT_HPP
#define DDBSH_SRC_PARSER_HELP_TEXT_HPP

#include <string>
#include <string_view>
#include <vector>

#include "parser/ast.hpp"

namespace ddbsh::engine {

/// The verbatim usage text for a HELP topic — exactly the bytes the original
/// printf emitted for that `help_command` production, including every embedded
/// and trailing newline.
std::string_view help_text(HelpTopic topic);

/// Split a HELP usage string into the ReportOutcome lines the Renderer joins.
/// The Renderer emits each line followed by '\n' (render_report), and every
/// HELP string ends with '\n', so splitting on '\n' and dropping the single
/// trailing empty fragment reproduces the original bytes exactly: text with N
/// newlines yields N lines, rendered back to the identical N-newline string.
inline std::vector<std::string> help_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            // No trailing newline (not expected for HELP text, but handled):
            // keep the final unterminated fragment.
            if (start < text.size()) {
                lines.emplace_back(text.substr(start));
            }
            break;
        }
        lines.emplace_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PARSER_HELP_TEXT_HPP
