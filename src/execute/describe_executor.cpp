// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// describe_executor.cpp — see describe_executor.hpp.

#include "execute/describe_executor.hpp"

#include <array>
#include <string>
#include <utility>

namespace ddbsh::engine {

namespace {

/// The scalar-type NAME the original emitted (ScalarAttributeTypeMapper): N/S/B.
std::string scalar_type_name(ScalarType t) {
    switch (t) {
        case ScalarType::n:
            return "N";
        case ScalarType::b:
            return "B";
        case ScalarType::s:
            break;
    }
    return "S";
}

/// The projection-type NAME the original emitted (ProjectionTypeMapper).
std::string projection_type_name(ProjectionType t) {
    switch (t) {
        case ProjectionType::keys_only:
            return "KEYS_ONLY";
        case ProjectionType::include:
            return "INCLUDE";
        case ProjectionType::all:
            break;
    }
    return "ALL";
}

/// Render a key schema as the original did (KeyTypeMapper name + " " + attr,
/// joined by ", ", in [HASH…, RANGE…] order): "HASH pk" or
/// "HASH pk, RANGE rk". A composite GSI key lists every element, e.g.
/// "HASH a, HASH b, RANGE c, RANGE d" (phase 4a).
std::string key_schema_text(const TableKeySchema& ks) {
    std::string out;
    bool first = true;
    for (const auto& h : ks.hash_keys) {
        out += (first ? "" : ", ");
        first = false;
        out += "HASH " + h;
    }
    for (const auto& r : ks.range_keys) {
        out += (first ? "" : ", ");
        first = false;
        out += "RANGE " + r;
    }
    return out;
}

}  // namespace

Result<ExecOutcome> execute_describe(const DescribeCmd& cmd, IDDBClient& client) {
    auto described = client.describe_table(DescribeTableIn{cmd.table});
    if (!described.has_value()) {
        // The original CDescribeHelper logs the failure and run() returns -1
        // with no stdout. We surface the error instead (D12 resiliency); no
        // in-scope golden checks DESCRIBE-on-missing output.
        return fail(described.error());
    }
    const DescribeTableOut& td = described.value();

    // The original fetches DescribeTimeToLive via CDescribeHelper but never
    // displays it in non-EXPLAIN mode (none of the __show_* helpers print TTL).
    // Replicate the call to preserve the original's API-call sequence (and
    // because the W6 EXPLAIN block prints this request); the result is ignored
    // here, and a TTL failure is non-fatal (the original's run() only checks
    // has_dtr()).
    (void)client.describe_time_to_live(DescribeTimeToLiveIn{cmd.table});

    ReportOutcome report;
    auto& lines = report.lines;

    // ---- __show_table_basic_info ------------------------------------------
    lines.push_back("Name: " + td.table_name + " (" + td.table_status + ")");
    lines.push_back("Key: " + key_schema_text(td.key_schema));
    {
        // "Attributes: " then, per attribute, ", " separator before all but the
        // first, each attribute printed as " <name>, <type>" (leading space) —
        // reproduces describe.cpp's printf sequence exactly, including the
        // double space after "Attributes:" and after each separator.
        std::string attrs = "Attributes: ";
        bool first = true;
        for (const auto& a : td.attribute_definitions) {
            if (!first) {
                attrs += ", ";
            } else {
                first = false;
            }
            attrs += " " + a.name + ", " + scalar_type_name(a.type);
        }
        lines.push_back(attrs);
    }
    lines.push_back("Created at: " + td.creation_date_time);
    lines.push_back("Table ARN: " + td.table_arn);
    lines.push_back("Table ID: " + td.table_id);
    lines.push_back("Table size (bytes): " + std::to_string(td.table_size_bytes));
    lines.push_back("Item Count: " + std::to_string(td.item_count));
    lines.push_back(std::string("Deletion Protection: ") +
                    (td.deletion_protection_enabled ? "Enabled" : "Disabled"));

    // ---- __show_billing_mode (returns table_is_on_demand) -----------------
    bool table_is_on_demand = false;
    auto provisioned_line = [&td]() {
        return "Billing Mode: Provisioned (" + std::to_string(td.provisioned_read_capacity_units) +
               " RCU, " + std::to_string(td.provisioned_write_capacity_units) + " WCU)";
    };
    if (td.billing_mode_summary.has_value()) {
        if (*td.billing_mode_summary == "PROVISIONED") {
            lines.push_back(provisioned_line());
        } else if (*td.billing_mode_summary == "PAY_PER_REQUEST") {
            lines.push_back("Billing Mode: On Demand");
            table_is_on_demand = true;
            if (td.on_demand_throughput_set) {
                lines.push_back("On Demand Throughput: (" +
                                std::to_string(td.on_demand_max_read_request_units) + " RCU, " +
                                std::to_string(td.on_demand_max_write_request_units) + " WCU)");
            }
        } else {
            // Unreachable in practice (only PROVISIONED/PAY_PER_REQUEST exist);
            // the original printed the raw enum int here.
            lines.push_back("Unknown Billing mode " + *td.billing_mode_summary + ".");
        }
    } else if (td.provisioned_throughput_set) {
        lines.push_back(provisioned_line());
    }

    // ---- __show_table_warm_throughput -------------------------------------
    if (td.warm_throughput.has_value()) {
        const auto& wt = *td.warm_throughput;
        lines.push_back("Warm Throughput: (" + std::to_string(wt.read_units_per_second) + " RCU, " +
                        std::to_string(wt.write_units_per_second) + " WCU, Status " + wt.status +
                        ")");
    }

    // ---- PITR (DescribeContinuousBackups) ---------------------------------
    // Best-effort: a failure skips the section (the original's IsSuccess()
    // guard), and a line is printed only for ENABLED or DISABLED.
    {
        auto pitr = client.describe_continuous_backups(DescribeContinuousBackupsIn{cmd.table});
        if (pitr.has_value()) {
            const auto& cb = pitr.value();
            if (cb.pitr_status == "ENABLED") {
                lines.push_back("PITR is Enabled: [" + cb.earliest_restorable_date_time + " to " +
                                cb.latest_restorable_date_time + "]");
            } else if (cb.pitr_status == "DISABLED") {
                lines.push_back("PITR is Disabled.");
            }
        }
    }

    // ---- __show_gsi -------------------------------------------------------
    if (!td.global_secondary_indexes.empty()) {
        for (const auto& gsi : td.global_secondary_indexes) {
            std::string line = "GSI " + gsi.index_name + ": ";
            line += "( " + key_schema_text(gsi.key_schema) + " ), ";
            if (!table_is_on_demand) {
                line += " Provisioned (" + std::to_string(gsi.provisioned_read_capacity_units) +
                        " RCU, " + std::to_string(gsi.provisioned_write_capacity_units) + " WCU), ";
            } else {
                line += "Billing Mode: On Demand (mirrors table), ";
                if (gsi.on_demand_throughput_set) {
                    line += "Throughput (" + std::to_string(gsi.on_demand_max_read_request_units) +
                            " RCU, " + std::to_string(gsi.on_demand_max_write_request_units) +
                            " WCU), ";
                }
            }
            if (gsi.projection_set) {
                if (gsi.projection.type == ProjectionType::include) {
                    std::string pkeys;
                    for (const auto& pk : gsi.projection.non_key_attributes) {
                        pkeys = pkeys.empty() ? pk : pkeys + ", " + pk;
                    }
                    line += "Projecting (INCLUDE) (" + pkeys + ")";
                } else {
                    line += "Projecting (" + projection_type_name(gsi.projection.type) + ")";
                }
            }
            line += ", Status: " + gsi.index_status;
            line += ", Backfilling: " + std::string(gsi.backfilling ? "YES" : "NO");
            if (gsi.warm_throughput.has_value()) {
                const auto& w = *gsi.warm_throughput;
                line += ", Warm Throughput: (" + std::to_string(w.read_units_per_second) +
                        " RCU, " + std::to_string(w.write_units_per_second) + " WCU, Status " +
                        w.status + ")";
            }
            lines.push_back(line);
        }
    } else {
        lines.push_back("GSI: None");
    }

    // ---- __show_lsi (all LSIs on ONE line — original quirk: the "\n" is after
    // the loop, so every LSI entry concatenates onto a single line) ----------
    if (!td.local_secondary_indexes.empty()) {
        std::string line;
        for (const auto& lsi : td.local_secondary_indexes) {
            line += "LSI " + lsi.index_name + ": ";
            line += "( " + key_schema_text(lsi.key_schema) + " ), ";
        }
        lines.push_back(line);
    } else {
        lines.push_back("LSI: None");
    }

    // ---- __show_stream ----------------------------------------------------
    if (td.stream_view_type.has_value()) {
        lines.push_back("Stream: " + *td.stream_view_type);
    } else {
        lines.push_back("Stream: Disabled");
    }

    // ---- __show_table_class -----------------------------------------------
    if (td.table_class_summary.has_value()) {
        lines.push_back("Table Class: " + *td.table_class_summary);
    } else {
        lines.push_back("Table Class: Has not been set.");
    }

    // ---- __show_table_sse -------------------------------------------------
    if (td.sse_status.has_value()) {
        lines.push_back("SSE: (" + *td.sse_status + ")");
    } else {
        lines.push_back("SSE: Not set");
    }

    // ---- __show_autoscaling (DescribeTableReplicaAutoScaling) -------------
    // Best-effort: a failure skips the section (the original's IsSuccess()
    // guard).
    {
        auto as = client.describe_table_replica_auto_scaling(
            DescribeTableReplicaAutoScalingIn{cmd.table});
        if (as.has_value()) {
            for (const auto& r : as.value().replicas) {
                lines.push_back("Replica Region: " + r.region_name +
                                " (Status: " + r.replica_status + ")");
            }
        }
    }

    return ExecOutcome{std::move(report)};
}

Result<ExecOutcome> execute_describe_explain(const DescribeCmd& cmd, IDDBClient& client) {
    // The four requests the DESCRIBE report would issue, in call order. The
    // original emitted DescribeTable + DescribeTimeToLive via CDescribeHelper
    // using strip_newlines (COMPACT, one line each), then DescribeContinuousBackups
    // and DescribeTableReplicaAutoScaling using explain_string (PRETTY). None of
    // these contacts the service — each request carries only the table name.
    using Format = ExplainBlock::Format;
    struct Spec {
        const char* api_name;
        ExplainRequest request;
        Format format;
    };
    const std::array<Spec, 4> specs{{
        {"DescribeTable", DescribeTableIn{cmd.table}, Format::compact},
        {"DescribeTimeToLive", DescribeTimeToLiveIn{cmd.table}, Format::compact},
        {"DescribeContinuousBackups", DescribeContinuousBackupsIn{cmd.table}, Format::pretty},
        {"DescribeTableReplicaAutoScaling", DescribeTableReplicaAutoScalingIn{cmd.table},
         Format::pretty},
    }};

    ExplainOutcome outcome;
    for (const auto& spec : specs) {
        auto body = client.explain_request(spec.request);
        if (!body.has_value()) {
            return fail(body.error());
        }
        outcome.blocks.push_back(ExplainBlock{spec.api_name, std::move(*body), spec.format});
    }
    return ExecOutcome{std::move(outcome)};
}

}  // namespace ddbsh::engine
