// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// show_tables_executor.cpp — see show_tables_executor.hpp.

#include "execute/show_tables_executor.hpp"

#include <regex>
#include <string>
#include <utility>

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

/// One SHOW TABLES line for `name`: just the name for BRIEF, or when either
/// DescribeTable or DescribeTimeToLive fails (the original's has_dtr() &&
/// has_ttl() guard in show_tables.cpp); else the full status row.
std::string table_row(const ShowTablesCmd& cmd, IDDBClient& client, const std::string& name) {
    if (cmd.brief) {
        return name;
    }
    auto described = client.describe_table(DescribeTableIn{name});
    if (!described.has_value()) {
        return name;
    }
    auto ttl = client.describe_time_to_live(DescribeTimeToLiveIn{name});
    if (!ttl.has_value()) {
        return name;
    }
    return status_row(name, described.value(), ttl.value());
}

}  // namespace

Result<void> execute_show_tables(const ShowTablesCmd& cmd, IDDBClient& client, OutcomeSink& sink) {
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

    // Read ListTables one page at a time and emit each row as soon as its
    // describe calls return, before the next row's calls and before the next
    // page is requested (P12, D-P12-11/-12) — the original show_tables.cpp
    // printed each row inside its page loop the same way. A ListTables failure
    // on a later page leaves the rows already emitted in place and returns the
    // Error after them (D-P12-14).
    ListTablesPageIn page_in;
    do {
        auto page = client.list_tables_page(page_in);
        if (!page.has_value()) {
            return fail(page.error());
        }
        for (const auto& name : page->table_names) {
            if (matcher.has_value() && !std::regex_match(name, *matcher)) {
                continue;
            }
            sink.emit(ExecOutcome{ReportOutcome{{table_row(cmd, client, name)}}});
        }
        page_in.exclusive_start_table_name = std::move(page->last_evaluated_table_name);
    } while (page_in.exclusive_start_table_name.has_value());
    return {};
}

}  // namespace ddbsh::engine
