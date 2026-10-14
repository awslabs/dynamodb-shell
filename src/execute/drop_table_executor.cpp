// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// drop_table_executor.cpp — see drop_table_executor.hpp.

#include "execute/drop_table_executor.hpp"

namespace ddbsh::engine {

Result<ExecOutcome> execute_drop_table(const DropTableCmd& cmd, IDDBClient& client) {
    // IF EXISTS: probe existence first and succeed silently when the table is
    // absent (the original's `if (!m_exists || table_exists(...))` guard — it
    // prints nothing and returns 0 for DROP TABLE IF EXISTS on a missing
    // table). A plain DROP skips the probe and lets DeleteTable surface the
    // not-found error.
    if (cmd.if_exists) {
        auto described = client.describe_table(DescribeTableIn{cmd.table});
        if (!described.has_value()) {
            // Table not found (or undescribable): nothing to drop, no output.
            return ExecOutcome{EmptyOutcome{}};
        }
    }

    auto deleted = client.delete_table(DeleteTableIn{cmd.table});
    if (!deleted.has_value()) {
        return fail(deleted.error());
    }

    // Block until the table is gone unless NOWAIT (arch §7). The waiter reports
    // a terminal control-plane failure as an Error.
    if (!cmd.nowait) {
        auto waited = client.wait_for_table_gone(cmd.table);
        if (!waited.has_value()) {
            return fail(waited.error());
        }
    }

    return ExecOutcome{StatusOutcome{"DROP"}};
}

Result<ExecOutcome> execute_drop_table_explain(const DropTableCmd& cmd, IDDBClient& client) {
    // IF EXISTS: the original's guard (`!m_exists || table_exists(...)`) runs
    // even when explaining, so EXPLAIN DROP IF EXISTS on a missing table emits
    // nothing (no DeleteTable block).
    if (cmd.if_exists) {
        auto described = client.describe_table(DescribeTableIn{cmd.table});
        if (!described.has_value()) {
            return ExecOutcome{EmptyOutcome{}};
        }
    }

    auto body = client.explain_request(DeleteTableIn{cmd.table});
    if (!body.has_value()) {
        return fail(body.error());
    }
    ExplainOutcome outcome;
    outcome.blocks.push_back(
        ExplainBlock{"DeleteTable", std::move(*body), ExplainBlock::Format::pretty});
    return ExecOutcome{std::move(outcome)};
}

}  // namespace ddbsh::engine
