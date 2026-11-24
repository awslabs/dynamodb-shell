// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// show_tables_executor.cpp — see show_tables_executor.hpp.

#include "execute/show_tables_executor.hpp"

#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

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

/// The report's columns for the output formats (P12, D-P12-27). BRIEF shows
/// only TableName.
const std::vector<std::string>& full_columns() {
    static const std::vector<std::string> columns{
        "TableName", "TableStatus", "BillingMode", "TableClass", "TableId",
        "TableArn",  "TimeToLive",  "GSICount",    "LSICount",   "VSICount"};
    return columns;
}

const std::vector<std::string>& brief_columns() {
    static const std::vector<std::string> columns{"TableName"};
    return columns;
}

/// One SHOW TABLES row in both shapes: the text line (the show_tables.cpp
/// printf, without the trailing newline the Renderer adds) and the same values
/// by column. A row whose describe failed carries only TableName.
struct TableRow {
    std::string line;
    ValueMap values;
};

TableRow name_only(const std::string& name) {
    return TableRow{name, ValueMap{{"TableName", Value::string_value(name)}}};
}

/// Build one full status row:
///   name | status | billing | class | id | arn | TTL ttl | GSI: n | LSI : m | VSI : k |
TableRow status_row(const std::string& name, const DescribeTableOut& td, const TimeToLiveOut& ttl) {
    TableRow out;
    std::string& row = out.line;
    row = name;
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
    out.values = ValueMap{
        {"TableName", Value::string_value(name)},
        {"TableStatus", Value::string_value(td.table_status)},
        {"BillingMode", Value::string_value(billing_mode_column(td))},
        {"TableClass", Value::string_value(table_class_column(td))},
        {"TableId", Value::string_value(td.table_id)},
        {"TableArn", Value::string_value(td.table_arn)},
        {"TimeToLive", Value::string_value(ttl_column(ttl))},
        {"GSICount", Value::number_value(std::to_string(td.global_secondary_indexes.size()))},
        {"LSICount", Value::number_value(std::to_string(td.local_secondary_indexes.size()))},
        {"VSICount", Value::number_value(std::to_string(td.vector_indexes.size()))},
    };
    return out;
}

/// One SHOW TABLES row for `name`: just the name for BRIEF, or when either
/// DescribeTable or DescribeTimeToLive fails (the original's has_dtr() &&
/// has_ttl() guard in show_tables.cpp); else the full status row.
TableRow table_row(const ShowTablesCmd& cmd, IDDBClient& client, const std::string& name) {
    if (cmd.brief) {
        return name_only(name);
    }
    auto described = client.describe_table(DescribeTableIn{name});
    if (!described.has_value()) {
        return name_only(name);
    }
    auto ttl = client.describe_time_to_live(DescribeTimeToLiveIn{name});
    if (!ttl.has_value()) {
        return name_only(name);
    }
    return status_row(name, described.value(), ttl.value());
}

/// One SHOW TABLES fragment: the row's line, and its values as a one-row
/// table. `group_end` marks the page's last name (the text-table format prints
/// a box there, D-P12-24). With no row, the fragment is an empty page end: it
/// renders nothing in the default format.
ExecOutcome fragment(const ShowTablesCmd& cmd, std::optional<TableRow> row, bool group_end) {
    ReportOutcome report;
    ReportTable table;
    table.columns = cmd.brief ? brief_columns() : full_columns();
    table.group_end = group_end;
    if (row.has_value()) {
        report.lines.push_back(std::move(row->line));
        table.rows.push_back(std::move(row->values));
    }
    report.table = std::move(table);
    return ExecOutcome{std::move(report)};
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
        // The output formats need to know where a page ends (the text table
        // prints one box per page, D-P12-24). The last name's fragment marks
        // it; when that name is filtered out by LIKE, or the page is empty, an
        // empty fragment marks it instead (it renders nothing by default).
        const auto& names = page->table_names;
        bool page_end_emitted = false;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (matcher.has_value() && !std::regex_match(names[i], *matcher)) {
                continue;
            }
            const bool last = i + 1 == names.size();
            sink.emit(fragment(cmd, table_row(cmd, client, names[i]), last));
            page_end_emitted = last;
        }
        if (!page_end_emitted) {
            sink.emit(fragment(cmd, std::nullopt, true));
        }
        page_in.exclusive_start_table_name = std::move(page->last_evaluated_table_name);
    } while (page_in.exclusive_start_table_name.has_value());
    return {};
}

}  // namespace ddbsh::engine
