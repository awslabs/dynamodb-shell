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
#include "ddbsh/value.hpp"

namespace ddbsh::render {

/// Format a single DynamoDB attribute Value into the exact ddbsh cell string:
/// a bare "simple" string, a quote-if-required string (space/tab/`,`/`:`/
/// newline/bracket/brace), a bare number, `TRUE`/`FALSE`, `NULL`, a `[..]`
/// list, or a `{k:v}` nested map (keys alpha-sorted, no space after the nested
/// colon) — arch §9. This is the SAME value formatter the Renderer uses for
/// item cells, exposed as a free function so an embedder building its own
/// output over typed rows can render one value in a single call without
/// reimplementing the Value::Type switch (D11). It is a pure function — no
/// globals, no locale dependence, no terminal.
std::string render_value(const Value& value);

/// Formats engine results into the exact bytes the golden .exp files expect.
class Renderer {
public:
    /// Success path: ExecOutcome -> exact output bytes (including trailing
    /// newline rules per outcome kind).
    std::string render(const ExecOutcome& outcome) const;

    /// The STDERR portion of a success outcome. Only a transaction's
    /// diagnostics (the non-reducible-SELECT message, D-2d-6) use this channel;
    /// every other outcome returns the empty string. The CLI writes render()
    /// to stdout and render_stderr() to stderr, so a transaction's `ABORT`
    /// (stdout) and its diagnostics (stderr) land on the right streams while
    /// the golden harness (which merges and -s sorts both) still matches. This
    /// is DATA on a success outcome — it does NOT flip the exit code
    /// (itxt-2 stays exit 0).
    std::string render_stderr(const ExecOutcome& outcome) const;

    /// Error path: byte-exact user-facing error string (tenet 5).
    std::string render_error(const Error& error) const;
};

}  // namespace ddbsh::render

#endif  // DDBSH_RENDERER_HPP
