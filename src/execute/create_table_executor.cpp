// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// create_table_executor.cpp — see create_table_executor.hpp.

#include "execute/create_table_executor.hpp"

namespace ddbsh::engine {

namespace {

/// A single (non-composite) key schema: at most one partition and at most one
/// sort attribute. Composite (multi-attribute) keys are a GSI-only feature
/// (phase 4a); base-table primary keys and LSIs must be single-key.
bool is_single_key(const KeySchema& ks) {
    return ks.hash.size() <= 1 && ks.range.size() <= 1;
}

/// Reject a composite key on a base-table primary key or an LSI (GSI-only
/// constraint, ratified D-4a-1/-8). Runs before the CreateTable request is
/// built so the shell never sends an illegal schema to the service. The error
/// strings are authored (no oracle; human-audited, D-4a-8).
Result<void> validate_key_schemas(const CreateTableCmd& cmd) {
    if (!is_single_key(cmd.key_schema)) {
        return fail(Errc::invalid_argument,
                    "Composite keys are supported only on GSIs. A table primary key must have "
                    "one partition key and an optional sort key.");
    }
    for (const auto& lsi : cmd.lsis) {
        if (!is_single_key(lsi.key_schema)) {
            return fail(Errc::invalid_argument,
                        "Composite keys are supported only on GSIs. LSI " + lsi.name +
                            " must have one partition key and an optional sort key.");
        }
    }
    return {};
}

/// Copy the parsed table definition into the client-seam CreateTableIn request.
/// The command-level flags (if_not_exists / nowait) are NOT part of the request
/// — they drive the executor's control flow, not the CreateTable call.
CreateTableIn make_request(const CreateTableCmd& cmd) {
    CreateTableIn request;
    request.table_name = cmd.table_name;
    request.attribute_definitions = cmd.attribute_definitions;
    request.key_schema = cmd.key_schema;
    request.billing = cmd.billing;
    request.warm = cmd.warm;
    request.gsis = cmd.gsis;
    request.lsis = cmd.lsis;
    request.stream = cmd.stream;
    request.sse = cmd.sse;
    request.table_class = cmd.table_class;
    request.deletion_protection = cmd.deletion_protection;
    request.tags = cmd.tags;
    return request;
}

}  // namespace

Result<ExecOutcome> execute_create_table(const CreateTableCmd& cmd, IDDBClient& client) {
    if (auto valid = validate_key_schemas(cmd); !valid.has_value()) {
        return fail(valid.error());
    }

    // IF NOT EXISTS: probe the table first. When it already exists, skip the
    // CreateTable call and succeed with the CREATE token (the original's
    // `if (m_if_not_exists && table_exists(...))` branch — it issues no request
    // and still prints "CREATE"). A describe failure (including not-found) is
    // treated as "does not exist, proceed to create", matching the original's
    // table_exists(), which returns false on any DescribeTable error.
    if (cmd.if_not_exists) {
        auto described = client.describe_table(DescribeTableIn{cmd.table_name});
        if (described.has_value()) {
            return ExecOutcome{StatusOutcome{"CREATE"}};
        }
    }

    auto created = client.create_table(make_request(cmd));
    if (!created.has_value()) {
        return fail(created.error());
    }

    // Block until the table is ACTIVE unless NOWAIT (arch §7). The waiter
    // reports a terminal control-plane failure as an Error.
    if (!cmd.nowait) {
        auto waited = client.wait_for_table_active(cmd.table_name);
        if (!waited.has_value()) {
            return fail(waited.error());
        }
    }

    return ExecOutcome{StatusOutcome{"CREATE"}};
}

Result<ExecOutcome> execute_create_table_explain(const CreateTableCmd& cmd, IDDBClient& client) {
    if (auto valid = validate_key_schemas(cmd); !valid.has_value()) {
        return fail(valid.error());
    }

    ExplainOutcome outcome;
    // The original prints the CREATE token even in EXPLAIN mode (outside the
    // explain branch, create_table.cpp).
    outcome.trailing_status = "CREATE";

    // IF NOT EXISTS: probe first. When the table exists, the else-branch (which
    // builds + serializes the request) is skipped — EXPLAIN emits only CREATE.
    if (cmd.if_not_exists) {
        auto described = client.describe_table(DescribeTableIn{cmd.table_name});
        if (described.has_value()) {
            return ExecOutcome{std::move(outcome)};
        }
    }

    auto body = client.explain_request(make_request(cmd));
    if (!body.has_value()) {
        return fail(body.error());
    }
    outcome.blocks.push_back(
        ExplainBlock{"CreateTable", std::move(*body), ExplainBlock::Format::pretty});
    return ExecOutcome{std::move(outcome)};
}

}  // namespace ddbsh::engine
