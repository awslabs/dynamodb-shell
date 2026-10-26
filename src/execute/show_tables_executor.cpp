// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// show_tables_executor.cpp — see show_tables_executor.hpp.

#include "execute/show_tables_executor.hpp"

#include <regex>
#include <string>

namespace ddbsh::engine {

namespace {

/// The billing-mode column, reproducing show_tables.cpp exactly: the mapped
/// BillingModeSummary name when present; else "PROVISIONED" when a
/// ProvisionedThroughput is set; else "PAY_PER_REQUEST*" (the "*" marks a value
/// inferred because the service returned neither).
std::string billing_mode_column(const DescribeTableOut& td) {
    if (td.billing_mode_summary.has_value()) {
        return *td.billing_mode_summary;
    }
    if (td.provisioned_throughput_set) {
        return "PROVISIONED";
    }
    return "PAY_PER_REQUEST*";
}

/// The table-class column: the mapped TableClassSummary name when present, else
/// "STANDARD*" (the "*" marks the inferred default), per show_tables.cpp.
std::string table_class_column(const DescribeTableOut& td) {
    if (td.table_class_summary.has_value()) {
        return *td.table_class_summary;
    }
    return "STANDARD*";
}

/// The TTL column text: the mapped status, with " (attr)" appended when TTL is
/// ENABLED or ENABLING (show_tables.cpp).
std::string ttl_column(const TimeToLiveOut& ttl) {
    std::string out = ttl.status;
    if (ttl.status == "ENABLED" || ttl.status == "ENABLING") {
        out += " (" + ttl.attribute_name + ")";
    }
    return out;
}

/// Build one full status row (the show_tables.cpp printf, without the trailing
/// newline the Renderer adds):
///   name | status | billing | class | id | arn | TTL ttl | GSI: n | LSI : m |
std::string status_row(const std::string& name, const DescribeTableOut& td,
                       const TimeToLiveOut& ttl) {
    std::string row = name;
    row += " | " + td.table_status;
    row += " | " + billing_mode_column(td);
    row += " | " + table_class_column(td);
    row += " | " + td.table_id;
    row += " | " + td.table_arn;
    row += " | TTL " + ttl_column(ttl);
    row += " | GSI: " + std::to_string(td.global_secondary_indexes.size());
    row += " | LSI : " + std::to_string(td.local_secondary_indexes.size());
    // Vector indexes (phase 4b, D-4b-10 Option A: always shown, even when 0 —
    // a human-approved divergence; re-records every SHOW TABLES golden).
    row += " | VSI : " + std::to_string(td.vector_indexes.size());
    row += " |";
    return row;
}

}  // namespace

Result<ExecOutcome> execute_show_tables(const ShowTablesCmd& cmd, IDDBClient& client) {
    // Build the LIKE matcher up front. An invalid pattern is reported as an
    // Error rather than throwing across the boundary (D12; the original
    // constructed std::regex(m_regexp) unguarded).
    std::optional<std::regex> matcher;
    if (!cmd.like.empty()) {
        try {
            matcher.emplace(cmd.like);
        } catch (const std::regex_error& e) {
            return fail(Errc::parse_error, std::string("Invalid LIKE pattern: ") + e.what());
        }
    }

    auto listed = client.list_tables(ListTablesIn{});
    if (!listed.has_value()) {
        return fail(listed.error());
    }

    ReportOutcome report;
    for (const auto& name : listed.value().table_names) {
        if (matcher.has_value() && !std::regex_match(name, *matcher)) {
            continue;
        }

        if (cmd.brief) {
            report.lines.push_back(name);
            continue;
        }

        // A non-brief row needs BOTH DescribeTable and DescribeTimeToLive to
        // succeed; if either fails the original prints just the table name
        // (has_dtr() && has_ttl() guard in show_tables.cpp).
        auto described = client.describe_table(DescribeTableIn{name});
        if (!described.has_value()) {
            report.lines.push_back(name);
            continue;
        }
        auto ttl = client.describe_time_to_live(DescribeTimeToLiveIn{name});
        if (!ttl.has_value()) {
            report.lines.push_back(name);
            continue;
        }

        report.lines.push_back(status_row(name, described.value(), ttl.value()));
    }

    return ExecOutcome{std::move(report)};
}

}  // namespace ddbsh::engine
