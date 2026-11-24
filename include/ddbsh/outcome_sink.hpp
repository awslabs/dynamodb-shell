// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// outcome_sink.hpp — the streaming output seam (AI-4, D-2c-12).
//
// The engine emits ExecOutcome fragments to an OutcomeSink in production order
// as they are produced, then returns success or an Error. This lets a command
// stream partial output before a mid-batch failure (e.g. a multi-row INSERT
// that writes rows 1..k, then fails on row k+1): the sink receives the k
// successful fragments and the caller then sees the Error — reproducing the
// original's behavior, which printed each row's status token as it wrote and
// the error after the already-printed tokens (arch §7, insert.cpp).
//
// The engine still writes to NO stream (tenet 3 / P2a-03): the sink is the
// EMBEDDER's. The CLI renders each fragment as it arrives (via the Renderer);
// the buffering Session::execute(stmt) convenience collects fragments into a
// single ExecOutcome for embedders that accept all-or-nothing. A Result<void>
// return carries the terminal success/failure; the fragments already emitted
// to the sink are the partial output.

#ifndef DDBSH_OUTCOME_SINK_HPP
#define DDBSH_OUTCOME_SINK_HPP

#include "ddbsh/exec_outcome.hpp"

namespace ddbsh {

/// A consumer of ExecOutcome fragments emitted by the engine during execution.
/// A single-outcome command emits exactly once. A command whose output comes
/// from several API responses emits one fragment per unit as soon as the
/// response that completes it arrives (P12, D-P12-1/D-P12-11): a SELECT one
/// per page, a multi-row INSERT one per written row, SHOW TABLES one per row,
/// SHOW BACKUPS one per ListBackups page, SHOW LIMITS and DESCRIBE one per
/// response group, a bulk UPDATE/DELETE with RETURN VALUES one per returned
/// item before its accounting line. A fragment can carry no rows: an empty
/// SELECT emits one SelectOutcome with no items (it carries the column_hint),
/// and SHOW TABLES / SHOW BACKUPS emit an empty ReportOutcome to mark a page
/// end that a LIKE filter emptied (or an empty result); the Renderer renders
/// these as nothing, so a sink that treats each fragment as content should
/// expect them. The embedder owns the sink and decides
/// what to do with each fragment (render it, buffer it, count it). The engine
/// never buffers to a stream itself.
///
/// Fragments emitted before a later API failure stay emitted; the caller sees
/// the Error after them (D-2c-12, D-P12-14).
class OutcomeSink {
public:
    virtual ~OutcomeSink() = default;

    /// Receive one outcome fragment, in production order. Called zero or more
    /// times per statement before Session::execute returns.
    virtual void emit(const ExecOutcome& fragment) = 0;

    /// The statement is over (P12, D-P12-16). Session::execute(stmt, sink)
    /// calls this exactly once per call, after the last emit(), on success and
    /// on failure, before it returns. A sink that holds state across fragments
    /// (an open result set in a formatted output) closes it here, so its
    /// closing bytes precede the error the caller prints. The default does
    /// nothing. If emit() throws, the exception propagates out of
    /// Session::execute and end_statement() is not called.
    virtual void end_statement() {}
};

}  // namespace ddbsh

#endif  // DDBSH_OUTCOME_SINK_HPP
