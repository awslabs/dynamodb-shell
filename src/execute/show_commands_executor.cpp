// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// show_commands_executor.cpp — see show_commands_executor.hpp.

#include "execute/show_commands_executor.hpp"

#include <string>
#include <utility>

namespace ddbsh::engine {

namespace {

/// Render a HASH/RANGE key schema as reparseable SHOW CREATE TABLE DDL. A
/// single-attribute role stays bare — `"attr" KEYTYPE` — byte-identical to the
/// original show_create_table.cpp get_key_description() so single-key goldens
/// do not regress. A composite (2+ attr) role is a parenthesized, comma-
/// separated list — `("a", "b") KEYTYPE` — matching the phase-4a SQLish
/// grammar so the emitted DDL round-trips through the parser. Roles are
/// separated by a leading space (all but the first) and a trailing comma (all
/// but the last): `"pk" HASH, ("a", "b") RANGE`.
std::string key_description(const TableKeySchema& ks) {
    std::vector<std::pair<std::vector<std::string>, std::string>> groups;  // (attrs, keytype)
    if (!ks.hash_keys.empty()) {
        groups.emplace_back(ks.hash_keys, "HASH");
    }
    if (!ks.range_keys.empty()) {
        groups.emplace_back(ks.range_keys, "RANGE");
    }
    std::string out;
    for (std::size_t ix = 0; ix < groups.size(); ++ix) {
        if (ix != 0) {
            out += " ";
        }
        const auto& attrs = groups[ix].first;
        if (attrs.size() == 1) {
            out += "\"" + attrs[0] + "\" " + groups[ix].second;
        } else {
            out += "(";
            for (std::size_t k = 0; k < attrs.size(); ++k) {
                if (k != 0) {
                    out += ", ";
                }
                out += "\"" + attrs[k] + "\"";
            }
            out += ") " + groups[ix].second;
        }
        if (ix != groups.size() - 1) {
            out += ",";
        }
    }
    return out;
}

std::string billing_mode_provisioned(long long rcu, long long wcu) {
    return "BILLING MODE PROVISIONED ( " + std::to_string(rcu) + " RCU, " + std::to_string(wcu) +
           " WCU ) ";
}

std::string billing_mode_on_demand() {
    return "BILLING MODE ON DEMAND ";
}

/// The DDL fragment for a projection (shared by GSI/LSI), matching the original
/// show_create_table.cpp switch.
std::string projection_ddl(const Projection& p) {
    switch (p.type) {
        case ProjectionType::all:
            return "PROJECTING ALL";
        case ProjectionType::keys_only:
            return "PROJECTING KEYS ONLY";
        case ProjectionType::include: {
            std::string out = "PROJECTING INCLUDE (";
            for (std::size_t iy = 0; iy < p.non_key_attributes.size(); ++iy) {
                if (iy != 0) {
                    out += ", ";
                }
                out += p.non_key_attributes[iy];
            }
            out += ")";
            return out;
        }
    }
    return "PROJECTING ALL";
}

std::string show_gsis(const std::vector<SecondaryIndexSchema>& gsis, bool table_on_demand) {
    if (gsis.empty()) {
        return {};
    }
    std::string out = "GSI (";
    for (std::size_t ix = 0; ix < gsis.size(); ++ix) {
        if (ix != 0) {
            out += ", ";
        }
        out +=
            "\"" + gsis[ix].index_name + "\" ON ( " + key_description(gsis[ix].key_schema) + " ) ";
        out += projection_ddl(gsis[ix].projection);
        out += " ";
        if (!table_on_demand) {
            out += billing_mode_provisioned(gsis[ix].provisioned_read_capacity_units,
                                            gsis[ix].provisioned_write_capacity_units);
        } else {
            out += billing_mode_on_demand();
        }
    }
    out += ") ";
    return out;
}

std::string show_lsis(const std::vector<SecondaryIndexSchema>& lsis) {
    if (lsis.empty()) {
        return {};
    }
    std::string out = "LSI (";
    for (std::size_t ix = 0; ix < lsis.size(); ++ix) {
        if (ix != 0) {
            out += ", ";
        }
        out +=
            "\"" + lsis[ix].index_name + "\" ON ( " + key_description(lsis[ix].key_schema) + " ) ";
        out += projection_ddl(lsis[ix].projection);
        out += " ";
    }
    out += ") ";
    return out;
}

/// Translate the SDK StreamViewType NAME ("NEW_IMAGE"/...) into the DDL words
/// the original stream_specification() emitted.
std::string stream_view_words(const std::string& mapped) {
    if (mapped == "NEW_IMAGE") {
        return "NEW IMAGE";
    }
    if (mapped == "OLD_IMAGE") {
        return "OLD IMAGE";
    }
    if (mapped == "KEYS_ONLY") {
        return "KEYS ONLY";
    }
    if (mapped == "NEW_AND_OLD_IMAGES") {
        return "BOTH IMAGES";
    }
    return mapped;
}

/// Reconstruct the CREATE TABLE DDL for SHOW CREATE TABLE, byte-for-byte with
/// the original show_create_table.cpp string builder (clause spacing incl.).
std::string reconstruct_ddl(const ShowCreateTableCmd& cmd, const DescribeTableOut& td) {
    std::string ctc = "CREATE TABLE ";
    if (cmd.if_not_exists) {
        ctc += "IF NOT EXISTS ";
    }
    if (cmd.nowait) {
        ctc += "NOWAIT ";
    }
    ctc += "\"" + td.table_name + "\" (";

    for (std::size_t ix = 0; ix < td.attribute_definitions.size(); ++ix) {
        if (ix != 0) {
            ctc += " ";
        }
        ctc += "\"" + td.attribute_definitions[ix].name + "\"";
        switch (td.attribute_definitions[ix].type) {
            case ScalarType::s:
                ctc += " string";
                break;
            case ScalarType::n:
                ctc += " number";
                break;
            case ScalarType::b:
                ctc += " binary";
                break;
        }
        if (ix != td.attribute_definitions.size() - 1) {
            ctc += ",";
        }
    }

    ctc += ") PRIMARY KEY (";
    ctc += key_description(td.key_schema);
    ctc += ") ";

    bool table_on_demand = false;
    if (td.billing_mode_summary.has_value()) {
        if (*td.billing_mode_summary == "PROVISIONED") {
            ctc += billing_mode_provisioned(td.provisioned_read_capacity_units,
                                            td.provisioned_write_capacity_units);
        } else if (*td.billing_mode_summary == "PAY_PER_REQUEST") {
            ctc += billing_mode_on_demand();
            table_on_demand = true;
        }
    } else if (td.provisioned_throughput_set) {
        ctc += billing_mode_provisioned(td.provisioned_read_capacity_units,
                                        td.provisioned_write_capacity_units);
    }

    ctc += show_gsis(td.global_secondary_indexes, table_on_demand);
    ctc += show_lsis(td.local_secondary_indexes);

    if (td.stream_specification_set) {
        if (td.stream_view_type.has_value()) {
            ctc += "STREAM ( " + stream_view_words(*td.stream_view_type) + " ) ";
        } else {
            ctc += "STREAM DISABLED ";
        }
    }

    // Table class: the original reads GetTableClassSummary().GetTableClass()
    // unconditionally; an absent summary defaults to STANDARD.
    const std::string table_class = td.table_class_summary.value_or("STANDARD");
    if (table_class == "STANDARD_INFREQUENT_ACCESS") {
        ctc += "TABLE CLASS STANDARD INFREQUENT ACCESS ";
    } else {
        ctc += "TABLE CLASS STANDARD ";
    }

    ctc += td.deletion_protection_enabled ? "DELETION PROTECTION ENABLED"
                                          : "DELETION PROTECTION DISABLED";
    return ctc;
}

}  // namespace

Result<ExecOutcome> execute_show_backups(const ShowBackupsCmd& cmd, IDDBClient& client) {
    ListBackupsIn request;
    if (!cmd.table.empty()) {
        request.table_name = cmd.table;
    }
    if (!cmd.backup_type.empty()) {
        request.backup_type = cmd.backup_type;
    }
    request.limit = 100;

    ReportOutcome report;
    for (;;) {
        auto page = client.list_backups(request);
        if (!page.has_value()) {
            return fail(page.error());
        }
        for (const auto& s : page->summaries) {
            report.lines.push_back("Table: " + s.table_name + ", Backup: " + s.backup_name +
                                   ", Status: " + s.backup_status + ", ARN: " + s.backup_arn +
                                   ", On: " + s.creation_datetime +
                                   ", Expires: " + s.expiry_datetime);
        }
        if (!page->last_evaluated_backup_arn.has_value()) {
            break;
        }
        request.exclusive_start_backup_arn = page->last_evaluated_backup_arn;
    }
    return ExecOutcome{std::move(report)};
}

Result<ExecOutcome> execute_show_limits(const ShowLimitsCmd&, IDDBClient& client,
                                        const std::string& region) {
    auto limits = client.describe_limits(DescribeLimitsIn{});
    if (!limits.has_value()) {
        return fail(limits.error());
    }

    ReportOutcome report;
    report.lines.push_back("Region: " + region);
    report.lines.push_back("Account Max (RCU, WCU): " + std::to_string(limits->account_max_rcu) +
                           ", " + std::to_string(limits->account_max_wcu));
    // NOTE: two spaces after the colon on this line — the original's literal
    // ("Table Max (RCU, WCU):  %lld, %lld").
    report.lines.push_back("Table Max (RCU, WCU):  " + std::to_string(limits->table_max_rcu) +
                           ", " + std::to_string(limits->table_max_wcu));

    auto listed = client.list_tables(ListTablesIn{});
    if (!listed.has_value()) {
        return fail(listed.error());
    }

    long long rcus = 0;
    long long wcus = 0;
    long long gsircus = 0;
    long long gsiwcus = 0;
    for (const auto& name : listed->table_names) {
        auto described = client.describe_table(DescribeTableIn{name});
        if (!described.has_value()) {
            // A table listed can vanish (or its describe can fail) before we
            // describe it — skip it, as the original did (dh.has_dtr() guard).
            continue;
        }
        const DescribeTableOut& td = described.value();
        const bool provisioned =
            (td.billing_mode_summary.has_value() && *td.billing_mode_summary == "PROVISIONED") ||
            td.provisioned_throughput_set;
        if (!provisioned) {
            continue;  // on-demand tables contribute 0
        }
        rcus += td.provisioned_read_capacity_units;
        wcus += td.provisioned_write_capacity_units;
        for (const auto& gsi : td.global_secondary_indexes) {
            gsircus += gsi.provisioned_read_capacity_units;
            gsiwcus += gsi.provisioned_write_capacity_units;
        }
    }

    report.lines.push_back("Total Tables (RCU, WCU): " + std::to_string(rcus) + ", " +
                           std::to_string(wcus));
    report.lines.push_back("Total GSI (RCU, WCU): " + std::to_string(gsircus) + ", " +
                           std::to_string(gsiwcus));
    report.lines.push_back("Total (RCU, WCU): " + std::to_string(rcus + gsircus) + ", " +
                           std::to_string(wcus + gsiwcus));
    return ExecOutcome{std::move(report)};
}

Result<ExecOutcome> execute_show_create_table(const ShowCreateTableCmd& cmd, IDDBClient& client) {
    auto described = client.describe_table(DescribeTableIn{cmd.table});
    if (!described.has_value()) {
        // The original table_exists() probe returns false on a missing table
        // and logs `The table "<t>" does not exist.` (return 1). Surface that
        // message as an Error (D12; 2f error text unpinned, D-2f-3).
        return fail(Errc::no_such_table, "The table \"" + cmd.table + "\" does not exist.");
    }
    const DescribeTableOut& td = described.value();

    if (td.table_status != "ACTIVE") {
        return fail(Errc::invalid_argument, "The table \"" + cmd.table + "\" is not ACTIVE.");
    }

    // The original CDescribeHelper always also fetches DescribeTimeToLive; the
    // DDL does not use it, and a TTL failure is non-fatal — call and ignore to
    // preserve the API-call sequence.
    (void)client.describe_time_to_live(DescribeTimeToLiveIn{cmd.table});

    ReportOutcome report;
    report.lines.push_back(reconstruct_ddl(cmd, td));
    return ExecOutcome{std::move(report)};
}

}  // namespace ddbsh::engine
