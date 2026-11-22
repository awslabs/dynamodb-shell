// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_cursor.hpp — a lazy, page-at-a-time pull cursor over a planned SELECT.
//
// PRIVATE engine header (not installed). D-P5-1 (phase 5b): the original
// ddbsh never buffered an entire Scan — CSelectCommand::run printed each item
// as it read each page, holding only the current page. The W5 executor
// (execute_select) regressed that by buffering the full item vector before
// returning. SelectRowCursor restores bounded-memory streaming: it owns the
// paging loop, the consumed-capacity totalizer, and the optional rate limiter,
// and yields ONE page at a time. Memory is bounded to a single page.
//
// Two consumers drain the same cursor (D2):
//   * the CLI / golden path (Session::execute(stmt, sink)) emits one
//     SelectOutcome{page_items, nullopt} fragment per page, then a final
//     SelectOutcome{{}, capacity} fragment if the user asked for capacity —
//     byte-identical to the old single SelectOutcome{all_items, capacity}
//     because the Renderer emits item lines then (iff present) the one
//     `SELECT (...)` line (arch §9); and
//   * the embedding path (Results::next_row) pages lazily, iterating rows
//     within each yielded page.
//
// Scope: Query/Scan (the unbounded paths) and GetItem (a single request,
// wrapped as a one-shot page) — i.e. every non-vector row SELECT. SearchVectors
// and read-transactions are single, naturally bounded requests and stay
// buffered (handled by execute_select directly), so the cursor never models
// them.
//
// D12: never aborts; a key-schema or service failure is returned as an Error.

#ifndef DDBSH_SRC_EXECUTE_SELECT_CURSOR_HPP
#define DDBSH_SRC_EXECUTE_SELECT_CURSOR_HPP

#include <optional>
#include <string>
#include <vector>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "execute/consumed_capacity_totalizer.hpp"
#include "execute/rate_limiter.hpp"
#include "planner/select_planner.hpp"

namespace ddbsh::engine {

/// A forward-only, page-at-a-time cursor over a planned GetItem/Query/Scan
/// SELECT. Move-only (it holds a client pointer and per-statement request
/// state); not thread-safe (one cursor per statement, same as the owning
/// Session). Memory is bounded to the current page.
class SelectRowCursor {
public:
    SelectRowCursor(SelectRowCursor&&) noexcept = default;
    SelectRowCursor& operator=(SelectRowCursor&&) noexcept = default;
    SelectRowCursor(const SelectRowCursor&) = delete;
    SelectRowCursor& operator=(const SelectRowCursor&) = delete;
    ~SelectRowCursor() = default;

    /// Fetch and return the next NON-EMPTY page of rows, nullptr at end, or an
    /// Error on a service failure. The returned pointer is owned by the cursor
    /// and valid only until the next next_page() call (or the cursor's
    /// destruction). Empty pages are totalized (capacity) and rate-limited but
    /// not yielded — only non-empty pages are returned, matching the original's
    /// "print each item as you read each page" with no spurious empty output.
    Result<const std::vector<ItemRow>*> next_page();

    /// The totalized consumed capacity, GATED on the user's RETURN CONSUMED
    /// CAPACITY request: nullopt when the user did not ask (a ratelimit forces
    /// the service to return capacity but the original does not display it,
    /// select.cpp display_consumed_capacity). Valid at any time; it is the
    /// final figure once next_page() has returned nullptr.
    std::optional<ConsumedCapacity> consumed_capacity() const;

private:
    friend Result<SelectRowCursor> open_select_cursor(const SelectCmd&, IDDBClient&,
                                                      const std::string&);

    explicit SelectRowCursor(IDDBClient& client) : client_(&client) {}

    IDDBClient* client_;
    SelectApi api_ = SelectApi::scan;
    GetItemIn get_item_{};
    QueryIn query_{};
    ScanIn scan_{};
    std::optional<RateLimiter> limiter_{};
    ConsumedCapacityTotalizer totalizer_{};
    bool report_capacity_ = false;  ///< user asked for RETURN CONSUMED CAPACITY

    std::vector<ItemRow> page_{};  ///< the most recently yielded page
    bool more_ = true;             ///< a further request may yield rows
    bool started_ = false;         ///< at least one request has been issued
};

/// Open a streaming cursor for a (non-EXPLAIN) SELECT. Plans the SAME access
/// pattern as execute_select (get_item/query/scan; the key schema is resolved
/// via describe_table), then captures the chosen request for lazy paging.
/// Precondition: the SELECT is NOT a vector search (the caller routes a vector
/// SELECT to execute_select, which buffers the single SearchVectors request);
/// a vector plan returns an Error here. `alias_prefix` seeds the one SymbolTable
/// the planner uses (Session::next_alias_prefix()).
Result<SelectRowCursor> open_select_cursor(const SelectCmd& cmd, IDDBClient& client,
                                           const std::string& alias_prefix);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_SELECT_CURSOR_HPP
