// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// alter_table_executor.cpp — see alter_table_executor.hpp.

#include "execute/alter_table_executor.hpp"

namespace ddbsh::engine {

namespace {

/// Copy the parsed ALTER definition into the client-seam UpdateTableIn request.
/// Only the fields the clause set are carried; the rest stay nullopt so the
/// adapter omits them from the UpdateTable payload (the original set exactly
/// the clause's attributes on one UpdateTableRequest — arch §7, D-2e-2).
UpdateTableIn make_request(const AlterTableCmd& cmd) {
    UpdateTableIn request;
    request.table_name = cmd.table;
    request.billing = cmd.billing;
    request.warm = cmd.warm;
    request.sse = cmd.sse;
    request.stream = cmd.stream;
    request.table_class = cmd.table_class;
    request.deletion_protection = cmd.deletion_protection;
    return request;
}

}  // namespace

Result<ExecOutcome> execute_alter_table(const AlterTableCmd& cmd, IDDBClient& client) {
    auto updated = client.update_table(make_request(cmd));
    if (!updated.has_value()) {
        return fail(updated.error());
    }
    return ExecOutcome{StatusOutcome{"ALTER"}};
}

Result<ExecOutcome> execute_alter_table_explain(const AlterTableCmd& cmd, IDDBClient& client) {
    auto body = client.explain_request(make_request(cmd));
    if (!body.has_value()) {
        return fail(body.error());
    }
    ExplainOutcome outcome;
    // The original ALTER prints the serialized request inside the explaining()
    // branch and returns before the "ALTER" printf, so EXPLAIN carries no
    // trailing status (contrast CREATE's "CREATE") — D-2e-11.
    outcome.blocks.push_back(
        ExplainBlock{"UpdateTable", std::move(*body), ExplainBlock::Format::pretty});
    return ExecOutcome{std::move(outcome)};
}

}  // namespace ddbsh::engine
