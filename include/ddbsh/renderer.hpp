// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// renderer.hpp — turns an ExecOutcome (or Error) into byte-exact output.
//
// The Renderer is the single place all formatting quirks live (P2a-03 §5):
// alpha-sorted item keys, the top-level ": " vs nested ":" asymmetry,
// quote_if_required, TRUE/FALSE/NULL, EXPLAIN 3-space indent, status tokens.
// It is a pure function of its input — no globals, no locale dependence, no
// terminal — so it is fully unit-testable without a backend.
//
// 2a scope: renders StatusOutcome / ReportOutcome / EmptyOutcome and the error
// path. Item/value/EXPLAIN formatting lands with the corresponding outcome
// arms in 2d/2e.

#ifndef DDBSH_RENDERER_HPP
#define DDBSH_RENDERER_HPP

#include <string>

#include "ddbsh/error.hpp"
#include "ddbsh/exec_outcome.hpp"

namespace ddbsh::render {

/// Formats engine results into the exact bytes the golden .exp files expect.
class Renderer {
public:
    /// Success path: ExecOutcome -> exact output bytes (including trailing
    /// newline rules per outcome kind).
    std::string render(const ExecOutcome& outcome) const;

    /// Error path: byte-exact user-facing error string (tenet 5).
    std::string render_error(const Error& error) const;
};

}  // namespace ddbsh::render

#endif  // DDBSH_RENDERER_HPP
