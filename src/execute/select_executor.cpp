// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_executor.cpp — see select_executor.hpp.

#include "execute/select_executor.hpp"

#include <utility>

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

    SelectOutcome outcome;
    ConsumedCapacityTotalizer totalizer;

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
                if (!result->last_evaluated_key.has_value() ||
                    result->last_evaluated_key->empty()) {
                    break;
                }
                request.exclusive_start_key = std::move(result->last_evaluated_key);
                // NOTE: ratelimit throttling (consume_reads/readlimit) is
                // deferred to a later sub-phase (D19); pagination itself is
                // honored here so multi-page result sets are complete.
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
                if (!result->last_evaluated_key.has_value() ||
                    result->last_evaluated_key->empty()) {
                    break;
                }
                request.exclusive_start_key = std::move(result->last_evaluated_key);
            }
            break;
        }
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

}  // namespace ddbsh::engine
