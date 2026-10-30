// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// items.cpp — ddbsh::items(), the row-extraction convenience for embedders.
//
// A pure, SDK-free read over an ExecOutcome: it pulls the item rows out of
// whichever arm carries them so an application loop never has to switch on the
// variant (D11). The row-bearing arms are SelectOutcome, TransactReadOutcome,
// and VectorSearchOutcome; MultiOutcome is flattened in production order; every
// other arm contributes nothing.

#include "ddbsh/exec_outcome.hpp"

#include <variant>

namespace ddbsh {

namespace {

// Append the rows carried by one outcome to `out`, recursing into a
// MultiOutcome's parts. A free helper so the recursion reuses the same visit.
void collect_items(const ExecOutcome& outcome, std::vector<ItemRow>& out) {
    std::visit(
        [&out](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, SelectOutcome>) {
                out.insert(out.end(), payload.items.begin(), payload.items.end());
            } else if constexpr (std::is_same_v<T, TransactReadOutcome>) {
                out.insert(out.end(), payload.items.begin(), payload.items.end());
            } else if constexpr (std::is_same_v<T, VectorSearchOutcome>) {
                for (const auto& row : payload.rows) {
                    out.push_back(row.item);
                }
            } else if constexpr (std::is_same_v<T, MultiOutcome>) {
                for (const auto& part : payload.parts) {
                    collect_items(part, out);
                }
            }
            // All other arms carry no item rows.
        },
        outcome);
}

}  // namespace

std::vector<ItemRow> items(const ExecOutcome& outcome) {
    std::vector<ItemRow> out;
    collect_items(outcome, out);
    return out;
}

}  // namespace ddbsh
