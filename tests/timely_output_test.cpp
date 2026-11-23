// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// timely_output_test.cpp — P12 slice S1: every command's output reaches the
// sink as soon as the API response that completes it arrives (D-P12-1,
// D-P12-11..16).
//
// A RecordingClient (a FakeDDBClient that logs each API call) and a LogSink
// (logs each emitted fragment, rendered, and each end_statement) share one
// event log, so each test asserts the exact interleaving of calls and emits:
// fragment k is in the log before API call k+1. Covered, per the P12 timing
// inventory (§1 of the P12 design):
//   T1  SHOW TABLES — one row per fragment, between the per-table describes,
//       ListTables read page by page (D-P12-12);
//   T2  SHOW BACKUPS — one fragment per ListBackups page;
//   T3  SHOW LIMITS — the header right after DescribeLimits;
//   T4  DESCRIBE — one fragment per response group;
//   T5  SELECT WITH RATELIMIT — page k emitted before the sleep (D-P12-13);
//   T7  bulk UPDATE/DELETE with RETURN VALUES — each item right after its
//       write, the accounting line last;
//   T9  bulk EXPLAIN UPDATE/DELETE — the read block before its read call;
//   T10 multi-row INSERT — unchanged, one status per row.
// Plus: end_statement() exactly once per execute, on success and on failure
// (D-P12-16); partial output on a later failure (D-P12-14); the buffering
// Session::execute(stmt) keeps each command's single-outcome shape and the
// streamed bytes equal the buffered bytes (D-P12-15); the CLI's
// FileRenderingSink has each fragment's bytes in its file before the next API
// call (it flushes, not only emits).

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "execute/select_cursor.hpp"
#include "fake_ddb_client.hpp"
#include "parser/driver.hpp"
#include "repl.hpp"

namespace {

using ddbsh::BackupSummaryRow;
using ddbsh::BulkWriteOutcome;
using ddbsh::ConsumedCapacity;
using ddbsh::DescribeTableOut;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::ExecOutcome;
using ddbsh::ExplainOutcome;
using ddbsh::ItemRow;
using ddbsh::ReportOutcome;
using ddbsh::Result;
using ddbsh::TableKeySchema;
using ddbsh::Value;
using ddbsh::ValueMap;

using Log = std::vector<std::string>;

// Replace each newline with '|' so a rendered fragment is one log entry.
std::string one_line(std::string s) {
    for (char& c : s) {
        if (c == '\n') {
            c = '|';
        }
    }
    return s;
}

// A FakeDDBClient that appends "call:<Op>[:<table>]" to a shared log before
// it serves each API call the tests below exercise, and runs an optional hook
// (the CLI-sink test samples its output file there).
class RecordingClient : public ddbsh::test::FakeDDBClient {
public:
    explicit RecordingClient(Log& log) : log_(log) {}

    std::function<void(const std::string&)> on_call;

    Result<ddbsh::ListTablesOut> list_tables(const ddbsh::ListTablesIn& in) override {
        record("ListTables");
        return FakeDDBClient::list_tables(in);
    }
    Result<ddbsh::ListTablesPageOut> list_tables_page(const ddbsh::ListTablesPageIn& in) override {
        record("ListTables");
        return FakeDDBClient::list_tables_page(in);
    }
    Result<DescribeTableOut> describe_table(const ddbsh::DescribeTableIn& in) override {
        record("DescribeTable:" + in.table_name);
        return FakeDDBClient::describe_table(in);
    }
    Result<ddbsh::TimeToLiveOut> describe_time_to_live(
        const ddbsh::DescribeTimeToLiveIn& in) override {
        record("DescribeTimeToLive:" + in.table_name);
        return FakeDDBClient::describe_time_to_live(in);
    }
    Result<ddbsh::ContinuousBackupsOut> describe_continuous_backups(
        const ddbsh::DescribeContinuousBackupsIn& in) override {
        record("DescribeContinuousBackups:" + in.table_name);
        return FakeDDBClient::describe_continuous_backups(in);
    }
    Result<ddbsh::TableReplicaAutoScalingOut> describe_table_replica_auto_scaling(
        const ddbsh::DescribeTableReplicaAutoScalingIn& in) override {
        record("DescribeTableReplicaAutoScaling:" + in.table_name);
        return FakeDDBClient::describe_table_replica_auto_scaling(in);
    }
    Result<ddbsh::ListBackupsOut> list_backups(const ddbsh::ListBackupsIn& in) override {
        record("ListBackups");
        return FakeDDBClient::list_backups(in);
    }
    Result<ddbsh::DescribeLimitsOut> describe_limits(const ddbsh::DescribeLimitsIn& in) override {
        record("DescribeLimits");
        return FakeDDBClient::describe_limits(in);
    }
    Result<ddbsh::QueryOut> query(const ddbsh::QueryIn& in) override {
        record("Query");
        return FakeDDBClient::query(in);
    }
    Result<ddbsh::ScanOut> scan(const ddbsh::ScanIn& in) override {
        record("Scan");
        return FakeDDBClient::scan(in);
    }
    Result<ddbsh::UpdateItemOut> update_item(const ddbsh::UpdateItemIn& in) override {
        record("UpdateItem");
        return FakeDDBClient::update_item(in);
    }
    Result<ddbsh::DeleteItemOut> delete_item(const ddbsh::DeleteItemIn& in) override {
        record("DeleteItem");
        return FakeDDBClient::delete_item(in);
    }
    Result<ddbsh::PutItemOut> put_item(const ddbsh::PutItemIn& in) override {
        record("PutItem");
        return FakeDDBClient::put_item(in);
    }

private:
    void record(const std::string& op) {
        if (on_call) {
            on_call(op);
        }
        log_.push_back("call:" + op);
    }
    Log& log_;
};

// Logs "emit:<rendered fragment>" per fragment and "end" per end_statement,
// and keeps each fragment for shape asserts.
struct LogSink : ddbsh::OutcomeSink {
    explicit LogSink(Log& l) : log(l) {}
    Log& log;
    std::vector<ExecOutcome> parts;
    int ends = 0;
    ddbsh::render::Renderer renderer;
    void emit(const ExecOutcome& fragment) override {
        parts.push_back(fragment);
        log.push_back("emit:" + one_line(renderer.render(fragment)));
    }
    void end_statement() override {
        ++ends;
        log.push_back("end");
    }
};

std::string render(const ExecOutcome& o) {
    return ddbsh::render::Renderer{}.render(o);
}

std::string render_all(const std::vector<ExecOutcome>& parts) {
    std::string out;
    for (const auto& p : parts) {
        out += render(p);
    }
    return out;
}

ddbsh::Session make_session(RecordingClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

// Run one statement through the streaming overload.
Result<void> stream(ddbsh::Session& session, const std::string& text, LogSink& sink) {
    auto stmt = session.parse(text);
    EXPECT_TRUE(stmt.has_value()) << text;
    if (!stmt.has_value()) {
        return ddbsh::fail(stmt.error());
    }
    return session.execute(*stmt, sink);
}

DescribeTableOut table(const std::string& name, TableKeySchema ks = TableKeySchema{{"pk"}, {}}) {
    DescribeTableOut t;
    t.table_name = name;
    t.table_status = "ACTIVE";
    t.key_schema = std::move(ks);
    return t;
}

Value num(const std::string& n) {
    return Value::number_value(n);
}

ItemRow pk_row(const std::string& n) {
    return ItemRow{ValueMap{{"pk", num(n)}}};
}

Error service_error(const std::string& message) {
    return Error{Errc::service_error, message, std::nullopt, std::nullopt};
}

// The SHOW TABLES status row for a fake table described with table() and the
// fake's default TTL (DISABLED).
std::string full_row(const std::string& name) {
    return name + " | ACTIVE | PAY_PER_REQUEST* | STANDARD* |  |  | TTL DISABLED | GSI: 0 | " +
           "LSI : 0 | VSI : 0 |";
}

// Three tables over two ListTables pages (a, b | c).
void seed_three_tables(RecordingClient& client) {
    client.set_table_names({"a", "b", "c"});
    client.set_list_tables_page_size(2);
    for (const char* name : {"a", "b", "c"}) {
        client.set_table(table(name));
    }
}

// ---- T1: SHOW TABLES --------------------------------------------------------

TEST(TimelyOutput, ShowTablesEmitsEachRowBeforeTheNextTablesCalls) {
    Log log;
    RecordingClient client(log);
    seed_three_tables(client);
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "show tables;", sink).has_value());

    const Log expected{
        "call:ListTables",
        "call:DescribeTable:a",
        "call:DescribeTimeToLive:a",
        "emit:" + full_row("a") + "|",
        "call:DescribeTable:b",
        "call:DescribeTimeToLive:b",
        "emit:" + full_row("b") + "|",
        "call:ListTables",  // page 2 is requested only after page 1's rows
        "call:DescribeTable:c",
        "call:DescribeTimeToLive:c",
        "emit:" + full_row("c") + "|",
        "end",
    };
    EXPECT_EQ(log, expected);
    ASSERT_EQ(client.list_tables_page_requests().size(), 2u);
    EXPECT_FALSE(client.list_tables_page_requests()[0].exclusive_start_table_name.has_value());
    EXPECT_EQ(client.list_tables_page_requests()[1].exclusive_start_table_name,
              std::optional<std::string>("2"));
}

TEST(TimelyOutput, ShowTablesLikeSkipsUnmatchedNamesAcrossPages) {
    Log log;
    RecordingClient client(log);
    seed_three_tables(client);
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    // The LIKE drops page 1's last name; the brief form makes no describe call.
    ASSERT_TRUE(stream(session, "show tables brief like \"[ac]\";", sink).has_value());

    const Log expected{"call:ListTables", "emit:a|", "call:ListTables", "emit:c|", "end"};
    EXPECT_EQ(log, expected);
}

TEST(TimelyOutput, ShowTablesFailureOnALaterPageKeepsTheEarlierRows) {
    // D-P12-14: rows already emitted stay emitted; the Error follows them.
    Log log;
    RecordingClient client(log);
    seed_three_tables(client);
    client.set_list_tables_error_at_page(1, service_error("throttled out"));
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    auto result = stream(session, "show tables brief;", sink);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "throttled out");

    const Log expected{"call:ListTables", "emit:a|", "emit:b|", "call:ListTables", "end"};
    EXPECT_EQ(log, expected);

    // The buffering overload still discards partial output on failure.
    auto buffered = session.run("show tables brief;");
    ASSERT_FALSE(buffered.has_value());
    EXPECT_EQ(buffered.error().message, "throttled out");
}

TEST(TimelyOutput, ShowTablesOverNoTablesEmitsNothing) {
    Log log;
    RecordingClient client(log);
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "show tables;", sink).has_value());
    EXPECT_EQ(log, (Log{"call:ListTables", "end"}));

    // Buffered: still the one (empty) ReportOutcome, as before P12.
    auto buffered = session.run("show tables;");
    ASSERT_TRUE(buffered.has_value());
    const auto* report = std::get_if<ReportOutcome>(&*buffered);
    ASSERT_NE(report, nullptr);
    EXPECT_TRUE(report->lines.empty());
}

// ---- T2: SHOW BACKUPS -------------------------------------------------------

BackupSummaryRow backup(const std::string& name) {
    return BackupSummaryRow{
        "t", name, "AVAILABLE", "arn:" + name, "2026-01-01T00:00:00Z", "2026-02-01T00:00:00Z"};
}

std::string backup_line(const std::string& name) {
    return "Table: t, Backup: " + name + ", Status: AVAILABLE, ARN: arn:" + name +
           ", On: 2026-01-01T00:00:00Z, Expires: 2026-02-01T00:00:00Z";
}

TEST(TimelyOutput, ShowBackupsEmitsEachPageBeforeTheNextRequest) {
    Log log;
    RecordingClient client(log);
    client.set_backup_pages({{backup("b1"), backup("b2")}, {backup("b3")}});
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "show backups;", sink).has_value());

    const Log expected{
        "call:ListBackups",
        "emit:" + backup_line("b1") + "|" + backup_line("b2") + "|",
        "call:ListBackups",
        "emit:" + backup_line("b3") + "|",
        "end",
    };
    EXPECT_EQ(log, expected);
}

TEST(TimelyOutput, ShowBackupsFailureOnPageTwoKeepsPageOne) {
    Log log;
    RecordingClient client(log);
    client.set_backup_pages({{backup("b1")}, {backup("b2")}});
    client.set_list_backups_error_at_page(1, service_error("ListBackups failed"));
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    auto result = stream(session, "show backups;", sink);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "ListBackups failed");
    const Log expected{"call:ListBackups", "emit:" + backup_line("b1") + "|", "call:ListBackups",
                       "end"};
    EXPECT_EQ(log, expected);
}

// ---- T3: SHOW LIMITS --------------------------------------------------------

const char* const kLimitsHeader =
    "emit:Region: us-east-1|Account Max (RCU, WCU): 80000, 80000|"
    "Table Max (RCU, WCU):  40000, 40000|";

void seed_limits(RecordingClient& client) {
    client.set_describe_limits(ddbsh::DescribeLimitsOut{80000, 80000, 40000, 40000});
    client.set_table_names({"a"});
    client.set_table(table("a"));
}

TEST(TimelyOutput, ShowLimitsEmitsTheHeaderBeforeListingTables) {
    Log log;
    RecordingClient client(log);
    seed_limits(client);
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "show limits;", sink).has_value());

    const Log expected{
        "call:DescribeLimits",
        kLimitsHeader,
        "call:ListTables",
        "call:DescribeTable:a",
        "emit:Total Tables (RCU, WCU): 0, 0|Total GSI (RCU, WCU): 0, 0|Total (RCU, WCU): 0, 0|",
        "end",
    };
    EXPECT_EQ(log, expected);
}

TEST(TimelyOutput, ShowLimitsListTablesFailureKeepsTheHeader) {
    Log log;
    RecordingClient client(log);
    seed_limits(client);
    client.set_list_tables_error(service_error("ListTables failed"));
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    auto result = stream(session, "show limits;", sink);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "ListTables failed");
    EXPECT_EQ(log, (Log{"call:DescribeLimits", kLimitsHeader, "call:ListTables", "end"}));
}

// ---- T4: DESCRIBE -----------------------------------------------------------

TEST(TimelyOutput, DescribeEmitsOneFragmentPerResponseGroup) {
    Log log;
    RecordingClient client(log);
    client.set_table(table("t"));
    client.set_auto_scaling("t", ddbsh::TableReplicaAutoScalingOut{{{"us-west-2", "ACTIVE"}}});
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "describe t;", sink).has_value());

    // Each group's fragment precedes the next group's call.
    ASSERT_EQ(log.size(), 8u);
    EXPECT_EQ(log[0], "call:DescribeTable:t");
    EXPECT_EQ(log[1], "call:DescribeTimeToLive:t");
    EXPECT_EQ(log[2].rfind("emit:Name: t (ACTIVE)|Key: HASH pk|", 0), 0u) << log[2];
    EXPECT_EQ(log[3], "call:DescribeContinuousBackups:t");
    EXPECT_EQ(log[4].rfind("emit:PITR is Disabled.|GSI: None|", 0), 0u) << log[4];
    EXPECT_EQ(log[5], "call:DescribeTableReplicaAutoScaling:t");
    EXPECT_EQ(log[6], "emit:Replica Region: us-west-2 (Status: ACTIVE)|");
    EXPECT_EQ(log[7], "end");

    // Streamed bytes equal the buffered report; the buffered shape is one
    // ReportOutcome.
    auto buffered = session.run("describe t;");
    ASSERT_TRUE(buffered.has_value());
    ASSERT_TRUE(std::holds_alternative<ReportOutcome>(*buffered));
    EXPECT_EQ(render(*buffered), render_all(sink.parts));
}

TEST(TimelyOutput, DescribeWithoutReplicasEmitsTwoFragments) {
    Log log;
    RecordingClient client(log);
    client.set_table(table("t"));
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "describe t;", sink).has_value());
    EXPECT_EQ(sink.parts.size(), 2u);  // the empty replica group emits nothing
    EXPECT_EQ(log.back(), "end");
}

TEST(TimelyOutput, DescribeOfAMissingTableEmitsNothing) {
    Log log;
    RecordingClient client(log);
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    auto result = stream(session, "describe nope;", sink);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(sink.parts.empty());
    EXPECT_EQ(sink.ends, 1);  // end_statement runs on failure too (D-P12-16)
}

// ---- T5: SELECT WITH RATELIMIT ----------------------------------------------

TEST(TimelyOutput, RatelimitedSelectReturnsEachPageBeforeItsSleep) {
    // D-P12-13: the cursor returns page k, and only the next next_page() call
    // sleeps before fetching page k+1. A fake clock advances by each sleep.
    Log log;
    RecordingClient client(log);
    client.set_table(table("t"));
    client.set_pages({{pk_row("1")}, {pk_row("2")}, {pk_row("3")}});
    ConsumedCapacity cc;
    cc.table_name = "t";
    cc.capacity_units = 2.0;  // 2 CU per page against a 1 RCU limit
    client.set_consumed_capacity(cc);

    double now = 0.0;
    auto clock = [&now] { return std::chrono::duration<double>(now); };
    auto sleep = [&now, &log](std::chrono::duration<double> d) {
        now += d.count();
        log.push_back("sleep");
    };

    ddbsh::engine::Driver driver("select * from t with ratelimit (1 rcu);");
    ASSERT_TRUE(driver.parse());
    auto ast = driver.release_result();
    ASSERT_NE(ast.ast(), nullptr);
    const auto& cmd = std::get<ddbsh::engine::SelectCmd>(ast.ast()->command);
    auto cursor = ddbsh::engine::open_select_cursor(cmd, client, "aaa", clock, sleep);
    ASSERT_TRUE(cursor.has_value());

    for (;;) {
        auto page = cursor->next_page();
        ASSERT_TRUE(page.has_value());
        if (*page == nullptr) {
            break;
        }
        log.push_back("emit:" + std::to_string((*page)->size()));
    }

    // One describe for the key schema, then: page, emit, sleep, page, ...
    // The last page has no successor, so no sleep follows it.
    const Log expected{
        "call:DescribeTable:t",
        "call:Scan",
        "emit:1",
        "sleep",
        "call:Scan",
        "emit:1",
        "sleep",
        "call:Scan",
        "emit:1",
    };
    EXPECT_EQ(log, expected);
}

// ---- T7: bulk UPDATE / DELETE with RETURN VALUES ----------------------------

void seed_two_pages(RecordingClient& client) {
    client.set_table(table("t"));
    client.set_pages({{pk_row("1"), pk_row("2")}, {pk_row("3")}});
}

TEST(TimelyOutput, BulkUpdateEmitsEachReturnedItemRightAfterItsWrite) {
    Log log;
    RecordingClient client(log);
    seed_two_pages(client);
    client.set_update_attributes(ItemRow{ValueMap{{"x", num("9")}}});
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(
        stream(session, "update t set x = 9 where x = 0 return values all new;", sink).has_value());

    const Log expected{
        "call:DescribeTable:t",
        "call:Scan",
        "call:UpdateItem",
        "emit:{x: 9}|",
        "call:UpdateItem",
        "emit:{x: 9}|",
        "call:Scan",
        "call:UpdateItem",
        "emit:{x: 9}|",
        "emit:UPDATE (3 read, 3 modified, 0 ccf)|",
        "end",
    };
    EXPECT_EQ(log, expected);

    // Buffered: one BulkWriteOutcome carrying all three items, not partial,
    // rendering the same bytes.
    auto buffered = session.run("update t set x = 9 where x = 0 return values all new;");
    ASSERT_TRUE(buffered.has_value());
    const auto* bulk = std::get_if<BulkWriteOutcome>(&*buffered);
    ASSERT_NE(bulk, nullptr);
    EXPECT_FALSE(bulk->partial);
    EXPECT_EQ(bulk->returned_items.size(), 3u);
    EXPECT_EQ(bulk->read, 3);
    EXPECT_EQ(render(*buffered), render_all(sink.parts));
}

TEST(TimelyOutput, BulkDeleteEmitsEachReturnedItemRightAfterItsDelete) {
    Log log;
    RecordingClient client(log);
    seed_two_pages(client);
    client.set_delete_attributes(ItemRow{ValueMap{{"pk", num("7")}}});
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(
        stream(session, "delete from t where x = 0 return values all old;", sink).has_value());

    const Log expected{
        "call:DescribeTable:t",
        "call:Scan",
        "call:DeleteItem",
        "emit:{pk: 7}|",
        "call:DeleteItem",
        "emit:{pk: 7}|",
        "call:Scan",
        "call:DeleteItem",
        "emit:{pk: 7}|",
        "emit:DELETE (3 read, 3 modified, 0 ccf)|",
        "end",
    };
    EXPECT_EQ(log, expected);

    auto buffered = session.run("delete from t where x = 0 return values all old;");
    ASSERT_TRUE(buffered.has_value());
    const auto* bulk = std::get_if<BulkWriteOutcome>(&*buffered);
    ASSERT_NE(bulk, nullptr);
    EXPECT_EQ(bulk->returned_items.size(), 3u);
    EXPECT_EQ(render(*buffered), render_all(sink.parts));
}

TEST(TimelyOutput, BulkUpdateFailureKeepsTheItemsAlreadyReturned) {
    // D-P12-14: the second write fails (not a ccf); the first item stays.
    Log log;
    RecordingClient client(log);
    seed_two_pages(client);
    client.set_update_attributes(ItemRow{ValueMap{{"x", num("9")}}});
    client.set_update_error_at(1, service_error("UpdateItem failed"));
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    auto result = stream(session, "update t set x = 9 where x = 0 return values all new;", sink);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "UpdateItem failed");
    const Log expected{
        "call:DescribeTable:t", "call:Scan",       "call:UpdateItem",
        "emit:{x: 9}|",         "call:UpdateItem", "end",
    };
    EXPECT_EQ(log, expected);
}

TEST(TimelyOutput, BulkUpdateWithoutReturnValuesEmitsOnlyTheAccountingLine) {
    Log log;
    RecordingClient client(log);
    seed_two_pages(client);
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "update t set x = 9 where x = 0;", sink).has_value());
    ASSERT_EQ(sink.parts.size(), 1u);
    EXPECT_EQ(render(sink.parts[0]), "UPDATE (3 read, 3 modified, 0 ccf)\n");
}

// ---- T9: bulk EXPLAIN UPDATE / DELETE ---------------------------------------

TEST(TimelyOutput, ExplainBulkUpdateEmitsEachReadBlockBeforeItsReadCall) {
    Log log;
    RecordingClient client(log);
    seed_two_pages(client);
    client.set_explain_body("{}");
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "explain update t set x = 9 where x = 0;", sink).has_value());

    const Log expected{
        "call:DescribeTable:t",
        "emit:Scan({})|",
        "call:Scan",
        "emit:UpdateItem({})|UpdateItem({})|",
        "emit:Scan({})|",
        "call:Scan",
        "emit:UpdateItem({})|",
        "end",
    };
    EXPECT_EQ(log, expected);

    // Buffered: one ExplainOutcome with all five blocks, same bytes.
    auto buffered = session.run("explain update t set x = 9 where x = 0;");
    ASSERT_TRUE(buffered.has_value());
    const auto* explain = std::get_if<ExplainOutcome>(&*buffered);
    ASSERT_NE(explain, nullptr);
    EXPECT_EQ(explain->blocks.size(), 5u);
    EXPECT_EQ(render(*buffered), render_all(sink.parts));
}

TEST(TimelyOutput, ExplainBulkDeleteEmitsTheScanBlockBeforeTheScan) {
    Log log;
    RecordingClient client(log);
    seed_two_pages(client);
    client.set_explain_body("{}");
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "explain delete from t where x = 0;", sink).has_value());

    // The original printed the Scan block once, before the loop.
    const Log expected{
        "call:DescribeTable:t",
        "emit:Scan({})|",
        "call:Scan",
        "emit:DeleteItem({})|DeleteItem({})|",
        "call:Scan",
        "emit:DeleteItem({})|",
        "end",
    };
    EXPECT_EQ(log, expected);

    auto buffered = session.run("explain delete from t where x = 0;");
    ASSERT_TRUE(buffered.has_value());
    const auto* explain = std::get_if<ExplainOutcome>(&*buffered);
    ASSERT_NE(explain, nullptr);
    EXPECT_EQ(explain->blocks.size(), 4u);
    EXPECT_EQ(render(*buffered), render_all(sink.parts));
}

// ---- T10: multi-row INSERT (unchanged) --------------------------------------

TEST(TimelyOutput, MultiRowInsertEmitsEachRowBeforeTheNextPut) {
    Log log;
    RecordingClient client(log);
    client.set_table(table("t"));
    ddbsh::Session session = make_session(client);
    LogSink sink(log);

    ASSERT_TRUE(stream(session, "insert into t (pk) values (1), (2);", sink).has_value());
    const Log expected{"call:DescribeTable:t", "call:PutItem", "emit:INSERT|",
                       "call:PutItem",         "emit:INSERT|", "end"};
    EXPECT_EQ(log, expected);
}

// ---- Buffered shapes (D-P12-15) ---------------------------------------------

TEST(TimelyOutput, BufferedReportsKeepTheirSingleOutcomeShape) {
    Log log;
    RecordingClient client(log);
    seed_three_tables(client);
    client.set_list_tables_page_size(1);
    client.set_backup_pages({{backup("b1")}, {backup("b2")}});
    client.set_describe_limits(ddbsh::DescribeLimitsOut{1, 2, 3, 4});
    ddbsh::Session session = make_session(client);

    for (const char* text : {"show tables;", "show backups;", "show limits;"}) {
        auto buffered = session.run(text);
        ASSERT_TRUE(buffered.has_value()) << text;
        ASSERT_TRUE(std::holds_alternative<ReportOutcome>(*buffered)) << text;

        LogSink sink(log);
        ASSERT_TRUE(stream(session, text, sink).has_value()) << text;
        EXPECT_GT(sink.parts.size(), 1u) << text;  // it did stream
        EXPECT_EQ(render(*buffered), render_all(sink.parts)) << text;
    }
    auto tables = session.run("show tables;");
    EXPECT_EQ(std::get<ReportOutcome>(*tables).lines.size(), 3u);
    auto limits = session.run("show limits;");
    EXPECT_EQ(std::get<ReportOutcome>(*limits).lines.size(), 6u);
}

// ---- end_statement (D-P12-16) -----------------------------------------------

TEST(TimelyOutput, EndStatementRunsOncePerExecuteAfterTheLastEmit) {
    Log log;
    RecordingClient client(log);
    ddbsh::Session session = make_session(client);

    for (const char* text : {"show version;", "show tables;", "select * from missing;"}) {
        LogSink sink(log);
        log.clear();
        (void)stream(session, text, sink);
        EXPECT_EQ(sink.ends, 1) << text;
        ASSERT_FALSE(log.empty()) << text;
        EXPECT_EQ(log.back(), "end") << text;
    }
}

// ---- The CLI sink flushes each fragment before the next API call ------------

TEST(TimelyOutput, CliSinkHasEachRowInItsFileBeforeTheNextDescribe) {
    Log log;
    RecordingClient client(log);
    seed_three_tables(client);
    ddbsh::Session session = make_session(client);

    std::FILE* out = std::tmpfile();
    std::FILE* err = std::tmpfile();
    ASSERT_NE(out, nullptr);
    ASSERT_NE(err, nullptr);
    // Sample the file's size, from the OS's view, at every API call.
    std::vector<std::pair<std::string, long>> sizes;
    client.on_call = [&](const std::string& op) {
        std::fseek(out, 0, SEEK_END);
        sizes.emplace_back(op, std::ftell(out));
    };

    ddbsh::render::Renderer renderer;
    ddbsh::cli::FileRenderingSink sink(renderer, out, err);
    auto stmt = session.parse("show tables;");
    ASSERT_TRUE(stmt.has_value());
    ASSERT_TRUE(session.execute(*stmt, sink).has_value());

    const long row = static_cast<long>(full_row("a").size() + 1);  // each row is the same width
    const std::vector<std::pair<std::string, long>> expected{
        {"ListTables", 0},
        {"DescribeTable:a", 0},
        {"DescribeTimeToLive:a", 0},
        {"DescribeTable:b", row},  // row a is in the file before b's calls
        {"DescribeTimeToLive:b", row},
        {"ListTables", 2 * row},
        {"DescribeTable:c", 2 * row},
        {"DescribeTimeToLive:c", 2 * row},
    };
    EXPECT_EQ(sizes, expected);

    std::fseek(out, 0, SEEK_END);
    EXPECT_EQ(std::ftell(out), 3 * row);
    std::fclose(out);
    std::fclose(err);
}

}  // namespace
