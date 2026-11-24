// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_executor.cpp — see select_executor.hpp.

#include "execute/select_executor.hpp"

#include <utility>

#include "execute/select_cursor.hpp"
#include "planner/select_planner.hpp"

namespace ddbsh::engine {

Result<ExecOutcome> execute_select(const SelectCmd& cmd, IDDBClient& client,
                                   const std::string& alias_prefix) {
    // SearchVectors (phase 4b) is a single, naturally bounded request (no
    // pagination, no key schema). It is buffered here and never modeled by the
    // streaming SelectRowCursor; the streaming and embedding paths route a
    // vector SELECT here too. Detect it on the AST (cmd.search_vector) exactly
    // as execute_select_streaming / open_select_cursor_for do, so the non-vector
    // path below plans once (via the cursor) without a second DescribeTable.
    // When cmd.search_vector is set, plan_select yields a search_vectors plan or
    // an already-formed Error (planner: a NEAREST on a non-vector/absent index,
    // CONSISTENT, or a bad TopK/vector fails), so plan.search_vectors is valid
    // on the success path.
    if (cmd.search_vector.has_value()) {
        auto planned = plan_select(cmd, client, alias_prefix);
        if (!planned.has_value()) {
            return fail(planned.error());
        }
        const SelectPlan& plan = *planned;
        auto result = client.search_vectors(plan.search_vectors);
        if (!result.has_value()) {
            return fail(result.error());
        }
        VectorSearchOutcome vout;
        for (auto& row : result->results) {
            vout.rows.push_back(VectorSearchOutcome::Row{std::move(row.item), row.score});
        }
        if (cmd.rcc != ReturnConsumedCapacity::none) {
            vout.vector_capacity = result->vector_capacity;
        }
        return ExecOutcome{std::move(vout)};
    }

    // Non-vector GetItem/Query/Scan: drain the SAME SelectRowCursor the
    // streaming (execute_select_streaming) and embedding (Results::next_row)
    // paths use, so the engine has exactly ONE pagination loop (D2). The
    // buffering contract is preserved — every page is folded into a single
    // SelectOutcome. consumed_capacity() is gated on RETURN CONSUMED CAPACITY
    // inside the cursor (nullopt when the user did not ask), reproducing the
    // original's display gating (select.cpp display_consumed_capacity); a
    // ratelimit alone shows nothing. The cursor builds a rate limiter only for
    // Query/Scan (never GetItem), matching the original's select path.
    auto opened = open_select_cursor(cmd, client, alias_prefix);
    if (!opened.has_value()) {
        return fail(opened.error());
    }
    SelectRowCursor cursor = std::move(*opened);

    SelectOutcome outcome;
    for (;;) {
        auto page = cursor.next_page();
        if (!page.has_value()) {
            return fail(page.error());
        }
        if (*page == nullptr) {
            break;
        }
        outcome.items.insert(outcome.items.end(), (*page)->begin(), (*page)->end());
    }
    outcome.consumed_capacity = cursor.consumed_capacity();
    outcome.column_hint = cursor.column_hint();

    return ExecOutcome{std::move(outcome)};
}

Result<void> execute_select_streaming(const SelectCmd& cmd, IDDBClient& client,
                                      const std::string& alias_prefix, OutcomeSink& sink) {
    // A vector SELECT (NEAREST) is a single, naturally bounded SearchVectors
    // request (or a planning error) — buffer it via execute_select and emit the
    // one outcome. Keeping this here means the cursor never models vectors.
    if (cmd.search_vector.has_value()) {
        auto buffered = execute_select(cmd, client, alias_prefix);
        if (!buffered.has_value()) {
            return fail(buffered.error());
        }
        sink.emit(*buffered);
        return {};
    }

    auto opened = open_select_cursor(cmd, client, alias_prefix);
    if (!opened.has_value()) {
        return fail(opened.error());
    }
    SelectRowCursor cursor = std::move(*opened);

    // Emit one SelectOutcome fragment per non-empty page; memory is bounded to
    // the current page. The Renderer prints each page's item lines, so the
    // concatenation is byte-identical to one SelectOutcome{all_items, ...}.
    // Every fragment carries the column hint the output formats use (P12).
    bool emitted = false;
    for (;;) {
        auto page = cursor.next_page();
        if (!page.has_value()) {
            return fail(page.error());
        }
        if (*page == nullptr) {
            break;
        }
        SelectOutcome fragment;
        fragment.items = **page;
        fragment.column_hint = cursor.column_hint();
        sink.emit(ExecOutcome{std::move(fragment)});
        emitted = true;
    }
    // A SELECT that found nothing emits one empty fragment, so a formatted
    // output still opens (and closes) its result set: `[]` in JSON, the header
    // in CSV (P12, design §4.4-§4.7). It renders nothing in the default
    // format, so the default bytes are unchanged.
    if (!emitted) {
        SelectOutcome empty;
        empty.column_hint = cursor.column_hint();
        sink.emit(ExecOutcome{std::move(empty)});
    }

    // After the item pages, emit the totalized capacity as its own fragment —
    // iff the user asked for it and the service returned any. The Renderer then
    // emits the one `SELECT (...)` line, exactly as the buffered path did
    // (arch §9); when the user did not ask, no fragment is emitted.
    if (auto cc = cursor.consumed_capacity(); cc.has_value()) {
        SelectOutcome capacity_fragment;
        capacity_fragment.consumed_capacity = std::move(cc);
        sink.emit(ExecOutcome{std::move(capacity_fragment)});
    }
    return {};
}

Result<ExecOutcome> execute_select_explain(const SelectCmd& cmd, IDDBClient& client,
                                           const std::string& alias_prefix) {
    // Plan the SAME access pattern execute_select would (this resolves the key
    // schema via describe_table — the one service call EXPLAIN SELECT makes,
    // arch §8), then serialize the chosen request instead of issuing it.
    auto planned = plan_select(cmd, client, alias_prefix);
    if (!planned.has_value()) {
        return fail(planned.error());
    }
    const SelectPlan& plan = *planned;

    std::string api_name;
    ExplainRequest request;
    switch (plan.api) {
        case SelectApi::get_item:
            api_name = "GetItem";
            request = plan.get_item;
            break;
        case SelectApi::query:
            api_name = "Query";
            request = plan.query;
            break;
        case SelectApi::scan:
            api_name = "Scan";
            request = plan.scan;
            break;
        case SelectApi::search_vectors:
            api_name = "SearchVectors";
            request = plan.search_vectors;
            break;
    }

    auto body = client.explain_request(request);
    if (!body.has_value()) {
        return fail(body.error());
    }

    ExplainOutcome outcome;
    outcome.blocks.push_back(
        ExplainBlock{std::move(api_name), std::move(*body), ExplainBlock::Format::pretty});
    return ExecOutcome{std::move(outcome)};
}

}  // namespace ddbsh::engine
