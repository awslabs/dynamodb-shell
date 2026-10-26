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
    request.attribute_definitions = cmd.attribute_definitions;
    request.gsi_updates = cmd.gsi_updates;
    request.vector_index_updates = cmd.vector_index_updates;
    return request;
}

/// The informational message the original emitted (logmsg) when a combined
/// `SET billing ... UPDATE GSI` targets an on-demand table (D-2e-6). Unpinned
/// by any golden; carried as a stderr diagnostic so it does not flip the exit
/// code. The original text came from logmsg; we drop its [file,line] prefix.
constexpr char kOnDemandGsiIgnored[] =
    "Setting table to on-demand, ignoring specifications for indices.";

/// Every ADD-GSI key schema must name at least one partition (HASH) attribute.
/// The generalized `key_schema` grammar rule (phase 4a) accepts a role list
/// with no HASH role; reject it client-side so the shell gives a clear error
/// rather than sending an empty HASH list to the service. Composite (multi-
/// attribute) GSI keys are allowed here (GSI-only feature); only the missing-
/// partition-key case is rejected. The error string is authored (no oracle;
/// human-audited, D-4a-8).
Result<void> validate_gsi_updates(const AlterTableCmd& cmd) {
    for (const auto& update : cmd.gsi_updates) {
        if (update.action == GsiUpdate::Action::create && update.key_schema &&
            update.key_schema->hash.empty()) {
            return fail(Errc::invalid_argument,
                        "GSI " + update.index_name + " must specify a partition (HASH) key.");
        }
        // G1 (phase 4b backlog): enforce the 4-attribute composite-key limit.
        if (update.action == GsiUpdate::Action::create && update.key_schema &&
            (update.key_schema->hash.size() > 4 || update.key_schema->range.size() > 4)) {
            return fail(Errc::invalid_argument,
                        "GSI " + update.index_name +
                            " key schema exceeds the 4-attribute limit per partition/sort key.");
        }
    }
    return {};
}

}  // namespace

Result<ExecOutcome> execute_alter_table(const AlterTableCmd& cmd, IDDBClient& client) {
    if (auto valid = validate_gsi_updates(cmd); !valid.has_value()) {
        return fail(valid.error());
    }
    auto updated = client.update_table(make_request(cmd));
    if (!updated.has_value()) {
        return fail(updated.error());
    }
    StatusOutcome status{"ALTER"};
    if (cmd.ignored_gsi_on_demand) {
        status.diagnostics.push_back(kOnDemandGsiIgnored);
    }
    return ExecOutcome{std::move(status)};
}

Result<ExecOutcome> execute_alter_table_explain(const AlterTableCmd& cmd, IDDBClient& client) {
    if (auto valid = validate_gsi_updates(cmd); !valid.has_value()) {
        return fail(valid.error());
    }
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

Result<ExecOutcome> execute_alter_ttl(const AlterTtlCmd& cmd, IDDBClient& client, bool explain) {
    // (1) DescribeTimeToLive is ALWAYS called first — even under EXPLAIN — and a
    // failure ends the command with no output (the original's has_ttl()==false
    // -> return -1 path). D-2e-7.
    auto probe = client.describe_time_to_live(DescribeTimeToLiveIn{cmd.table});
    if (!probe.has_value()) {
        return fail(probe.error());
    }

    // (2) Disabling an already-disabled table short-circuits to ALTER with NO
    // UpdateTimeToLive call. The original performs this check BEFORE its
    // explaining() branch, so it fires even when explaining — reproduced here
    // (so EXPLAIN of a redundant disable emits the ALTER token, not a block).
    if (probe->status == "DISABLED" && !cmd.enabled) {
        return ExecOutcome{StatusOutcome{"ALTER"}};
    }

    // (3) Build the UpdateTimeToLive request: enable uses the user-supplied
    // attribute; disable reuses the existing attribute name from the probe.
    UpdateTimeToLiveIn request;
    request.table_name = cmd.table;
    request.enabled = cmd.enabled;
    request.attribute_name = cmd.enabled ? cmd.attribute : probe->attribute_name;

    if (explain) {
        // EXPLAIN serializes ONLY the UpdateTimeToLive request (the live probe
        // above is not serialized), with NO trailing status — D-2e-11.
        auto body = client.explain_request(request);
        if (!body.has_value()) {
            return fail(body.error());
        }
        ExplainOutcome outcome;
        outcome.blocks.push_back(
            ExplainBlock{"UpdateTimeToLive", std::move(*body), ExplainBlock::Format::pretty});
        return ExecOutcome{std::move(outcome)};
    }

    auto updated = client.update_time_to_live(request);
    if (!updated.has_value()) {
        return fail(updated.error());
    }
    return ExecOutcome{StatusOutcome{"ALTER"}};
}

Result<ExecOutcome> execute_alter_pitr(const AlterPitrCmd& cmd, IDDBClient& client, bool explain) {
    UpdateContinuousBackupsIn request;
    request.table_name = cmd.table;
    request.enabled = cmd.enabled;

    if (explain) {
        // EXPLAIN serializes the UpdateContinuousBackups request under the
        // quirky label `UpdateContinuousBackupsRequest(...)` — the ONLY EXPLAIN
        // label the original gives a `Request` suffix (pitr.cpp printf) — with
        // NO trailing status token (D-2e-11).
        auto body = client.explain_request(request);
        if (!body.has_value()) {
            return fail(body.error());
        }
        ExplainOutcome outcome;
        outcome.blocks.push_back(ExplainBlock{"UpdateContinuousBackupsRequest", std::move(*body),
                                              ExplainBlock::Format::pretty});
        return ExecOutcome{std::move(outcome)};
    }

    auto updated = client.update_continuous_backups(request);
    if (!updated.has_value()) {
        return fail(updated.error());
    }
    return ExecOutcome{StatusOutcome{"ALTER"}};
}

Result<ExecOutcome> execute_alter_replica(const AlterReplicaCmd& cmd, IDDBClient& client,
                                          bool explain) {
    // A replica ALTER is an UpdateTable carrying exactly one
    // ReplicationGroupUpdate (the original CUpdateTableReplica::run builds one
    // UpdateTableRequest with a single ReplicationGroupUpdate). One UpdateTableIn
    // backs both the plain SET path and this replica path (D-2e-12): here only
    // the table name and the replica carrier are set; the billing/warm/sse/...
    // fields stay unset.
    UpdateTableIn request;
    request.table_name = cmd.table;
    request.replica = cmd.replica;

    if (explain) {
        // The original prints `UpdateTable(%s)` inside the explaining() branch
        // (NOT a `Request`-suffixed label — that quirk is unique to PITR) and
        // returns before the "ALTER" printf, so EXPLAIN carries NO trailing
        // status (D-2e-11).
        auto body = client.explain_request(request);
        if (!body.has_value()) {
            return fail(body.error());
        }
        ExplainOutcome outcome;
        outcome.blocks.push_back(
            ExplainBlock{"UpdateTable", std::move(*body), ExplainBlock::Format::pretty});
        return ExecOutcome{std::move(outcome)};
    }

    auto updated = client.update_table(request);
    if (!updated.has_value()) {
        return fail(updated.error());
    }
    return ExecOutcome{StatusOutcome{"ALTER"}};
}

}  // namespace ddbsh::engine
