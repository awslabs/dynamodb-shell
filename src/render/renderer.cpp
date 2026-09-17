// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// renderer.cpp — byte-exact formatting of engine results (2a subset).
//
// The Renderer owns ALL output formatting so byte-compat lives in one place
// (P2a-03 §5). This 2a implementation covers the walking-skeleton outcome arms
// (StatusOutcome / ReportOutcome / EmptyOutcome) and the error path. Item,
// value, and EXPLAIN formatting land with their outcome arms in 2d/2e; the
// top-level ": " vs nested ":" asymmetry and quote_if_required are implemented
// there against the ported goldens.

#include "ddbsh/renderer.hpp"

#include <variant>

namespace ddbsh::render {

namespace {

/// A status token renders as the token followed by a newline (arch §7/§9).
std::string render_status(const StatusOutcome& s) {
    return s.token + "\n";
}

/// A report renders its lines joined by newlines, each terminated by a
/// newline (arch §9 multi-line reports).
std::string render_report(const ReportOutcome& r) {
    std::string out;
    for (const auto& line : r.lines) {
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace

std::string Renderer::render(const ExecOutcome& outcome) const {
    return std::visit(
        [](const auto& payload) -> std::string {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, StatusOutcome>) {
                return render_status(payload);
            } else if constexpr (std::is_same_v<T, ReportOutcome>) {
                return render_report(payload);
            } else if constexpr (std::is_same_v<T, EmptyOutcome>) {
                return {};
            } else {
                static_assert(sizeof(T) == 0, "unhandled ExecOutcome arm");
            }
        },
        outcome);
}

std::string Renderer::render_error(const Error& error) const {
    // Byte-exact user string (tenet 5); the CLI writes this to stderr. The
    // per-Errc mapping to the original's exact strings is completed as each
    // command family lands; the message field already carries that string.
    return error.message + "\n";
}

}  // namespace ddbsh::render
