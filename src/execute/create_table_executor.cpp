// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// create_table_executor.cpp — see create_table_executor.hpp.

#include "execute/create_table_executor.hpp"

namespace ddbsh::engine {

namespace {

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

}  // namespace ddbsh::engine
