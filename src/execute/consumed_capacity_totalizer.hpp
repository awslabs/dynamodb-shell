// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// consumed_capacity_totalizer.hpp — sum consumed-capacity across pages.
//
// PRIVATE engine header (not installed). SDK-free port of the original
// CConsumedCapacityTotalizer (arch §10): it sums the optional capacity figures
// across paginated Query/Scan responses. The original asserted the table name
// stayed constant across pages; per D12 we do not assert on service-derived
// data — we seed the table name from the first response that carries one and
// sum the numeric fields.
//
// Shared by select_executor.cpp (the buffering SELECT path) and
// select_cursor.cpp (the streaming/lazy SELECT path) so there is exactly ONE
// totalizer implementation.

#ifndef DDBSH_SRC_EXECUTE_CONSUMED_CAPACITY_TOTALIZER_HPP
#define DDBSH_SRC_EXECUTE_CONSUMED_CAPACITY_TOTALIZER_HPP

#include <optional>

#include "ddbsh/iddbclient.hpp"

namespace ddbsh::engine {

/// Sums the optional capacity figures across paginated Query/Scan responses.
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

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_CONSUMED_CAPACITY_TOTALIZER_HPP
