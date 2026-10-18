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
/// A single-outcome command emits exactly once; a multi-row INSERT emits once
/// per written row; a bulk UPDATE/DELETE emits its one BulkWriteOutcome. The
/// embedder owns the sink and decides what to do with each fragment (render it,
/// buffer it, count it). The engine never buffers to a stream itself.
class OutcomeSink {
public:
    virtual ~OutcomeSink() = default;

    /// Receive one outcome fragment, in production order. Called zero or more
    /// times per statement before Session::execute returns.
    virtual void emit(const ExecOutcome& fragment) = 0;
};

}  // namespace ddbsh

#endif  // DDBSH_OUTCOME_SINK_HPP
