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

/// Every key schema — base-table primary key, LSI, or GSI — must name at least
/// one partition (HASH) attribute. The generalized `key_schema` grammar rule
/// (phase 4a) accepts a role list with no HASH role (e.g. `( a range )`), which
/// the original's three fixed alternatives rejected at parse time; validate it
/// client-side so the shell gives a clear error instead of sending an empty
/// HASH list to the service. The error strings are authored (no oracle;
/// human-audited, D-4a-8).
Result<void> validate_key_schemas(const CreateTableCmd& cmd) {
    if (cmd.key_schema.hash.empty()) {
        return fail(Errc::invalid_argument,
                    "A table primary key must specify a partition (HASH) key.");
    }
    if (!is_single_key(cmd.key_schema)) {
        return fail(Errc::invalid_argument,
                    "Composite keys are supported only on GSIs. A table primary key must have "
                    "one partition key and an optional sort key.");
    }
    for (const auto& lsi : cmd.lsis) {
        if (lsi.key_schema.hash.empty()) {
            return fail(Errc::invalid_argument,
                        "LSI " + lsi.name + " must specify a partition (HASH) key.");
        }
        if (!is_single_key(lsi.key_schema)) {
            return fail(Errc::invalid_argument,
                        "Composite keys are supported only on GSIs. LSI " + lsi.name +
                            " must have one partition key and an optional sort key.");
        }
    }
    for (const auto& gsi : cmd.gsis) {
        if (gsi.key_schema.hash.empty()) {
            return fail(Errc::invalid_argument,
                        "GSI " + gsi.name + " must specify a partition (HASH) key.");
        }
        // G1 (phase 4b backlog): DynamoDB allows up to 4 partition and 4 sort
        // attributes on a composite GSI key; reject more client-side with a
        // clear message instead of letting the service reject it.
        if (gsi.key_schema.hash.size() > 4 || gsi.key_schema.range.size() > 4) {
            return fail(Errc::invalid_argument,
                        "GSI " + gsi.name +
                            " key schema exceeds the 4-attribute limit per partition/sort key.");
        }
    }
    // Vector-index validation (phase 4b, D-4b-8). The vector attribute is an
    // L-of-N, not a scalar key type, so it must NOT be declared in the top
    // attribute-type list; the SEARCH KEY (HASH/INLINE_FILTER) and PROJECTING
    // INCLUDE attributes, by contrast, MUST be declared there. Authored error
    // messages (no oracle; the original predates vector search).
    auto is_declared = [&cmd](const std::string& name) {
        for (const auto& a : cmd.attribute_definitions) {
            if (a.name == name) {
                return true;
            }
        }
        return false;
    };
    for (const auto& vi : cmd.vector_indexes) {
        if (is_declared(vi.vector_attr)) {
            return fail(Errc::invalid_argument,
                        "The vector attribute " + vi.vector_attr + " of vector index " +
                            vi.index_name +
                            " must not be declared in the attribute list (it is a vector, "
                            "not a scalar key).");
        }
        for (const auto& e : vi.search_schema) {
            if (!is_declared(e.attr)) {
                return fail(Errc::invalid_argument,
                            "The search-key attribute " + e.attr + " of vector index " +
                                vi.index_name + " must be declared in the attribute list.");
            }
        }
        if (vi.projection.type == ProjectionType::include) {
            for (const auto& a : vi.projection.non_key_attributes) {
                if (!is_declared(a)) {
                    return fail(Errc::invalid_argument,
                                "The projected attribute " + a + " of vector index " +
                                    vi.index_name + " must be declared in the attribute list.");
                }
            }
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
    request.vector_indexes = cmd.vector_indexes;
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
