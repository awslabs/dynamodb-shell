// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// stream_fragments.hpp — small emit helpers for the streaming bulk writes.
//
// PRIVATE engine header (not installed). A bulk UPDATE/UPSERT/DELETE emits its
// output as it is produced (P12, D-P12-11): under EXPLAIN one ExplainOutcome
// per serialized request group, and with RETURN VALUES one partial
// BulkWriteOutcome per returned item before the final accounting fragment
// (D-P12-15). These helpers build those fragments so the UPDATE and DELETE
// executors shape them identically.

#ifndef DDBSH_SRC_EXECUTE_STREAM_FRAGMENTS_HPP
#define DDBSH_SRC_EXECUTE_STREAM_FRAGMENTS_HPP

#include <string>
#include <utility>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/outcome_sink.hpp"

namespace ddbsh::engine {

/// Emit one pretty EXPLAIN block (`api_name(body)`) as its own fragment.
inline void emit_explain_block(OutcomeSink& sink, std::string api_name, std::string body) {
    ExplainOutcome out;
    out.blocks.push_back(
        ExplainBlock{std::move(api_name), std::move(body), ExplainBlock::Format::pretty});
    sink.emit(ExecOutcome{std::move(out)});
}

/// Emit one RETURN VALUES item of a bulk write as a partial BulkWriteOutcome
/// (renders the item line only; the accounting line follows at the end).
inline void emit_returned_item(OutcomeSink& sink, BulkWriteOutcome::Verb verb, ItemRow item) {
    BulkWriteOutcome part;
    part.verb = verb;
    part.returned_items.push_back(std::move(item));
    part.partial = true;
    sink.emit(ExecOutcome{std::move(part)});
}

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_STREAM_FRAGMENTS_HPP
