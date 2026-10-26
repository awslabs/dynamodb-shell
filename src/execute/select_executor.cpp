// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_executor.cpp — see select_executor.hpp.

#include "execute/select_executor.hpp"

#include <utility>

#include "execute/rate_limiter.hpp"
#include "planner/select_planner.hpp"

namespace ddbsh::engine {

namespace {

/// SDK-free port of CConsumedCapacityTotalizer (arch §10): sums the optional
/// capacity figures across paginated Query/Scan responses. The original
/// asserted the table name stayed constant across pages; per D12 we do not
/// assert on service-derived data — we seed the table name from the first
/// response that carries one and sum the numeric fields.
class ConsumedCapacityTotalizer {
public:
    void add(const std::optional<ConsumedCapacity>& cc) {
        if (!cc.has_value()) {
            return;
        }
        seen_ = true;
        if (!total_.table_name.has_value() && cc->table_name.has_value()) {
            total_.table_name = cc->table_name;
        }
        accumulate(total_.capacity_units, cc->capacity_units);
        accumulate(total_.read_capacity_units, cc->read_capacity_units);
        accumulate(total_.write_capacity_units, cc->write_capacity_units);
    }

    /// The totalized capacity, or nullopt if no response carried any.
    std::optional<ConsumedCapacity> get() const {
        if (!seen_) {
            return std::nullopt;
        }
        return total_;
    }

private:
    static void accumulate(std::optional<double>& sink, const std::optional<double>& v) {
        if (v.has_value()) {
            sink = sink.value_or(0.0) + *v;
        }
    }

    bool seen_ = false;
    ConsumedCapacity total_;
};

}  // namespace

Result<ExecOutcome> execute_select(const SelectCmd& cmd, IDDBClient& client,
                                   const std::string& alias_prefix) {
    auto planned = plan_select(cmd, client, alias_prefix);
    if (!planned.has_value()) {
        return fail(planned.error());
    }
    const SelectPlan& plan = *planned;

    // SearchVectors (phase 4b) is a single request (no pagination, no key
    // schema); handle it before the paginated Query/Scan machinery and return a
    // VectorSearchOutcome (item + score per row).
    if (plan.api == SelectApi::search_vectors) {
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

    SelectOutcome outcome;
    ConsumedCapacityTotalizer totalizer;

    // A WITH RATELIMIT clause meters the read bucket across pages (select.cpp
    // do_query/do_scan: consume_reads per page, readlimit before the next). The
    // GetItem path never constructs a limiter (one request, nothing to
    // paginate — the original's select path builds no limiter for GetItem).
    std::optional<RateLimiter> limiter;
    if (cmd.ratelimit.has_value()) {
        limiter.emplace(cmd.ratelimit->rcu, cmd.ratelimit->wcu);
    }

    switch (plan.api) {
        case SelectApi::get_item: {
            auto result = client.get_item(plan.get_item);
            if (!result.has_value()) {
                return fail(result.error());
            }
            if (result->item.has_value()) {
                outcome.items.push_back(std::move(*result->item));
            }
            totalizer.add(result->consumed_capacity);
            break;
        }
        case SelectApi::query: {
            QueryIn request = plan.query;
            while (true) {
                auto result = client.query(request);
                if (!result.has_value()) {
                    return fail(result.error());
                }
                for (auto& item : result->items) {
                    outcome.items.push_back(std::move(item));
                }
                totalizer.add(result->consumed_capacity);
                if (limiter.has_value()) {
                    limiter->consume_reads(result->consumed_capacity);
                }
                if (!result->last_evaluated_key.has_value() ||
                    result->last_evaluated_key->empty()) {
                    break;
                }
                request.exclusive_start_key = std::move(result->last_evaluated_key);
                // Throttle before fetching the next page (do_query: readlimit()
                // after setting the next ExclusiveStartKey). Output is unchanged.
                if (limiter.has_value()) {
                    limiter->readlimit();
                }
            }
            break;
        }
        case SelectApi::scan: {
            ScanIn request = plan.scan;
            while (true) {
                auto result = client.scan(request);
                if (!result.has_value()) {
                    return fail(result.error());
                }
                for (auto& item : result->items) {
                    outcome.items.push_back(std::move(item));
                }
                totalizer.add(result->consumed_capacity);
                if (limiter.has_value()) {
                    limiter->consume_reads(result->consumed_capacity);
                }
                if (!result->last_evaluated_key.has_value() ||
                    result->last_evaluated_key->empty()) {
                    break;
                }
                request.exclusive_start_key = std::move(result->last_evaluated_key);
                if (limiter.has_value()) {
                    limiter->readlimit();
                }
            }
            break;
        }
        case SelectApi::search_vectors:
            break;  // handled above via early return
    }

    // Display consumed capacity only when the user asked for it (RETURN
    // CONSUMED CAPACITY). A ratelimit forces the request to TOTAL but the
    // original gates the display on the user's `returns`, not the request's
    // (select.cpp display_consumed_capacity), so a ratelimit alone shows
    // nothing.
    if (cmd.rcc != ReturnConsumedCapacity::none) {
        outcome.consumed_capacity = totalizer.get();
    }

    return ExecOutcome{std::move(outcome)};
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
