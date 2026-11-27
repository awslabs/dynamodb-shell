// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_cursor.cpp — see select_cursor.hpp.

#include "execute/select_cursor.hpp"

#include <utility>

namespace ddbsh::engine {

Result<SelectRowCursor> open_select_cursor(const SelectCmd& cmd, IDDBClient& client,
                                           const std::string& alias_prefix,
                                           RateLimiter::Clock clock, RateLimiter::Sleep sleep) {
    auto planned = plan_select(cmd, client, alias_prefix);
    if (!planned.has_value()) {
        return fail(planned.error());
    }
    SelectPlan& plan = *planned;

    if (plan.api == SelectApi::search_vectors) {
        // A vector SELECT is a single, naturally bounded SearchVectors request;
        // the caller must route it to execute_select (buffered). The cursor
        // never models it. This is a programming error (the caller pre-screens
        // vector SELECTs), surfaced as an Error rather than silently mishandled.
        return fail(Errc::internal_error,
                    "open_select_cursor does not handle a vector search; route to execute_select");
    }

    SelectRowCursor cursor(client);
    cursor.api_ = plan.api;
    cursor.get_item_ = std::move(plan.get_item);
    cursor.query_ = std::move(plan.query);
    cursor.query_upper_ = std::move(plan.query_upper);
    cursor.scan_ = std::move(plan.scan);
    cursor.report_capacity_ = (cmd.rcc != ReturnConsumedCapacity::none);
    cursor.column_hint_ = std::move(plan.column_hint);

    // A WITH RATELIMIT clause meters the read bucket across pages (select.cpp
    // do_query/do_scan). GetItem is a single request — the original builds no
    // limiter for the GetItem path — so only Query/Scan construct one.
    if (cmd.ratelimit.has_value() && plan.api != SelectApi::get_item) {
        cursor.limiter_.emplace(cmd.ratelimit->rcu, cmd.ratelimit->wcu, std::move(clock),
                                std::move(sleep));
    }

    return cursor;
}

Result<const std::vector<ItemRow>*> SelectRowCursor::next_page() {
    // Loop until a non-empty page is produced or the source is exhausted.
    // Every page (even an empty one) is totalized and rate-limited exactly as
    // the original's do_query/do_scan loop; only non-empty pages are yielded so
    // the caller emits output solely for pages that carry rows (matching
    // "print each item as you read each page", arch §7/§10).
    while (more_) {
        // The ratelimit sleep owed for the previous page is taken here, at the
        // start of the next fetch, not at the end of the previous page (P12,
        // D-P12-13). The sleeps and their order against the API calls are the
        // original's (do_query/do_scan: readlimit() after setting the next
        // ExclusiveStartKey, before the next request); the difference is that
        // the previous page has already been returned to the caller, and so
        // emitted, before the sleep.
        if (throttle_pending_) {
            throttle_pending_ = false;
            limiter_->readlimit();
        }

        std::vector<ItemRow> items;
        std::optional<ConsumedCapacity> cc;
        std::optional<ValueMap> last_evaluated_key;

        switch (api_) {
            case SelectApi::get_item: {
                auto result = client_->get_item(get_item_);
                if (!result.has_value()) {
                    return fail(result.error());
                }
                if (result->item.has_value()) {
                    items.push_back(std::move(*result->item));
                }
                cc = std::move(result->consumed_capacity);
                more_ = false;  // GetItem is a single request
                break;
            }
            case SelectApi::query: {
                auto result = client_->query(query_);
                if (!result.has_value()) {
                    return fail(result.error());
                }
                items = std::move(result->items);
                cc = std::move(result->consumed_capacity);
                last_evaluated_key = std::move(result->last_evaluated_key);
                break;
            }
            case SelectApi::scan: {
                auto result = client_->scan(scan_);
                if (!result.has_value()) {
                    return fail(result.error());
                }
                items = std::move(result->items);
                cc = std::move(result->consumed_capacity);
                last_evaluated_key = std::move(result->last_evaluated_key);
                break;
            }
            case SelectApi::search_vectors:
                // Unreachable: open_select_cursor rejects a vector plan.
                more_ = false;
                break;
        }

        started_ = true;
        totalizer_.add(cc);
        if (limiter_.has_value()) {
            limiter_->consume_reads(cc);
        }

        // Decide whether a further page may exist, and set the next cursor key
        // (Query/Scan only — GetItem already cleared more_).
        if (api_ == SelectApi::query || api_ == SelectApi::scan) {
            if (!last_evaluated_key.has_value() || last_evaluated_key->empty()) {
                if (api_ == SelectApi::query && query_upper_.has_value()) {
                    // The lower leg of a not-equal split is exhausted: the next
                    // fetch starts the upper leg from its first page (P15,
                    // D-P15-8). It is metered like any next page: the read
                    // bucket is charged before its first request.
                    query_ = std::move(*query_upper_);
                    query_upper_.reset();
                    throttle_pending_ = limiter_.has_value();
                } else {
                    more_ = false;
                }
            } else {
                if (api_ == SelectApi::query) {
                    query_.exclusive_start_key = std::move(last_evaluated_key);
                } else {
                    scan_.exclusive_start_key = std::move(last_evaluated_key);
                }
                // Owe a throttle before the next fetch (do_query/do_scan:
                // readlimit() after setting the next ExclusiveStartKey). It is
                // taken at the top of the next iteration or next_page() call,
                // after this page is returned. Output bytes are unaffected —
                // the limiter only sleeps (arch §10).
                throttle_pending_ = limiter_.has_value();
            }
        }

        if (!items.empty()) {
            page_ = std::move(items);
            return &page_;
        }
        // Empty page: keep draining (if more pages) or fall out and return
        // nullptr. The capacity/throttle for this page was already applied.
    }
    return nullptr;
}

std::optional<ConsumedCapacity> SelectRowCursor::consumed_capacity() const {
    if (!report_capacity_) {
        return std::nullopt;
    }
    return totalizer_.get();
}

}  // namespace ddbsh::engine
