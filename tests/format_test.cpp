// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// format_test.cpp — the opt-in output formats (P12 S4, design §4, test 7).
//
// Offline, against hand-built outcomes and a FakeDDBClient-backed Session:
//   * decompose() for every ExecOutcome arm;
//   * JSON string escapes and the RFC 8259 number rule;
//   * both JSON flavors for all ten value types, and the cell text;
//   * CSV quoting, the header, and the header re-emitted when columns grow;
//   * the text table: widths in code points, escaped control characters,
//     right-aligned numbers, one box per API page (including the empty page
//     end a LIKE leaves), and the header-only box;
//   * the empty result in every format;
//   * a failure after a result set: the set closes before the error;
//   * the default format through FormattingSink equals the Renderer for every
//     arm, on both streams;
//   * the CLI's output directives and their precedence.

#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/format.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"
#include "repl.hpp"

namespace {

using ddbsh::BulkWriteOutcome;
using ddbsh::ExecOutcome;
using ddbsh::ExplainBlock;
using ddbsh::ExplainOutcome;
using ddbsh::ItemRow;
using ddbsh::MultiOutcome;
using ddbsh::ReportOutcome;
using ddbsh::ReportTable;
using ddbsh::SelectOutcome;
using ddbsh::StatusOutcome;
using ddbsh::TransactOutcome;
using ddbsh::TransactReadOutcome;
using ddbsh::Value;
using ddbsh::ValueMap;
using ddbsh::VectorSearchOutcome;
using ddbsh::WriteOutcome;
using ddbsh::render::ColumnTracker;
using ddbsh::render::FormatOptions;
using ddbsh::render::FormattingSink;
using ddbsh::render::JsonFlavor;
using ddbsh::render::OutputFormat;
using ddbsh::render::OutputPart;
using ddbsh::render::RowsPart;
using ddbsh::render::Stream;
using ddbsh::render::TextPart;

Value s(std::string v) {
    return Value::string_value(std::move(v));
}
Value n(std::string v) {
    return Value::number_value(std::move(v));
}
ItemRow row(ValueMap attrs) {
    return ItemRow{std::move(attrs)};
}
/// One SELECT page fragment.
ExecOutcome page(std::vector<ItemRow> items) {
    SelectOutcome sel;
    sel.items = std::move(items);
    return ExecOutcome{std::move(sel)};
}

/// A FormattingSink whose writer records "<O|E>:<text>" pieces and the plain
/// stdout text.
struct Capture {
    std::string out;
    std::string err;
    std::string log;  ///< both streams, in order, each piece tagged
    FormattingSink sink;

    explicit Capture(FormatOptions options)
        : sink(options, [this](Stream stream, std::string_view text) {
              (stream == Stream::out ? out : err) += text;
              log += (stream == Stream::out ? "O:" : "E:") + std::string(text);
          }) {}
};

FormatOptions fmt(OutputFormat f, JsonFlavor flavor = JsonFlavor::plain) {
    return FormatOptions{f, flavor};
}

std::string formatted(const ExecOutcome& outcome, OutputFormat f,
                      JsonFlavor flavor = JsonFlavor::plain) {
    return ddbsh::render::render_formatted(outcome, fmt(f, flavor));
}

/// One value of every type, as one item.
ItemRow all_types() {
    Value b = Value::binary_value("aGk=");
    Value t = Value::bool_value(true);
    Value z = Value::null_value();
    Value l;
    l.type = Value::Type::L;
    l.list = {n("1"), s("x")};
    Value m;
    m.type = Value::Type::M;
    m.map = {{"y", n("2")}, {"x", s("v")}};
    return row({{"s", s("str")},
                {"n", n("1.5")},
                {"b", b},
                {"t", t},
                {"z", z},
                {"l", l},
                {"m", m},
                {"ss", Value::string_set({"a", "b"})},
                {"ns", Value::number_set({"1", "2"})},
                {"bs", Value::binary_set({"AA==", "AQ=="})}});
}

// ---- ids ------------------------------------------------------------------

TEST(FormatIds, ParseAndNameEveryFormatAndFlavor) {
    EXPECT_EQ(ddbsh::render::parse_output_format("default"), OutputFormat::standard);
    EXPECT_EQ(ddbsh::render::parse_output_format("JSON"), OutputFormat::json);
    EXPECT_EQ(ddbsh::render::parse_output_format("Csv"), OutputFormat::csv);
    EXPECT_EQ(ddbsh::render::parse_output_format("table"), OutputFormat::table);
    EXPECT_FALSE(ddbsh::render::parse_output_format("html").has_value());
    EXPECT_FALSE(ddbsh::render::parse_output_format("").has_value());
    EXPECT_EQ(ddbsh::render::output_format_id(OutputFormat::standard), "default");
    EXPECT_EQ(ddbsh::render::output_format_id(OutputFormat::table), "table");
    EXPECT_EQ(ddbsh::render::parse_json_flavor("plain"), JsonFlavor::plain);
    EXPECT_EQ(ddbsh::render::parse_json_flavor("DynamoDB"), JsonFlavor::dynamodb);
    EXPECT_FALSE(ddbsh::render::parse_json_flavor("typed").has_value());
    EXPECT_EQ(ddbsh::render::json_flavor_id(JsonFlavor::dynamodb), "dynamodb");
}

// ---- decompose --------------------------------------------------------------

TEST(Decompose, SelectIsRowsThenTheCapacityLine) {
    SelectOutcome sel;
    sel.items = {row({{"a", n("1")}}), ItemRow{}};
    sel.column_hint = {"a"};
    ddbsh::ConsumedCapacity cc;
    cc.table_name = "t";
    cc.capacity_units = 0.5;
    sel.consumed_capacity = cc;
    auto parts = ddbsh::render::decompose(ExecOutcome{sel});
    ASSERT_EQ(parts.size(), 2u);
    const auto& rows = std::get<RowsPart>(parts[0]);
    EXPECT_EQ(rows.rows.size(), 1u);  // the empty item is skipped, as the Renderer does
    EXPECT_EQ(rows.columns, (std::vector<std::string>{"a"}));
    EXPECT_FALSE(rows.fixed_columns);
    EXPECT_EQ(std::get<TextPart>(parts[1]).out, "SELECT (t, 0.5, 0, 0)\n");
}

TEST(Decompose, AnEmptySelectIsStillARowsPart) {
    auto parts = ddbsh::render::decompose(ExecOutcome{SelectOutcome{}});
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_TRUE(std::get<RowsPart>(parts[0]).rows.empty());
}

TEST(Decompose, VectorSearchAddsAScoreColumnLast) {
    VectorSearchOutcome v;
    v.rows = {{row({{"b", n("2")}, {"a", n("1")}}), 0.25}, {row({{"score", s("x")}}), -1.5}};
    auto parts = ddbsh::render::decompose(ExecOutcome{v});
    ASSERT_EQ(parts.size(), 1u);
    const auto& rows = std::get<RowsPart>(parts[0]);
    // `score` is taken by an item attribute, so the column is `_score`.
    EXPECT_EQ(rows.columns, (std::vector<std::string>{"a", "b", "score", "_score"}));
    EXPECT_TRUE(rows.fixed_columns);
    EXPECT_EQ(formatted(ExecOutcome{v}, OutputFormat::json),
              "[\n  {\"a\": 1, \"b\": 2, \"_score\": 0.25},\n  {\"score\": \"x\", \"_score\": "
              "-1.5}\n]\n");
}

TEST(Decompose, TransactReadKeepsAMissAsAnEmptyRow) {
    TransactReadOutcome t;
    t.items = {row({{"a", n("1")}}), ItemRow{}};
    auto parts = ddbsh::render::decompose(ExecOutcome{t});
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(std::get<RowsPart>(parts[0]).rows.size(), 2u);
    EXPECT_EQ(formatted(ExecOutcome{t}, OutputFormat::json), "[\n  {\"a\": 1},\n  {}\n]\n");
}

TEST(Decompose, WriteIsTheTokenThenTheReturnedItem) {
    WriteOutcome w;
    w.tokens = {"REPLACE", "REPLACE"};
    w.items = {std::nullopt, row({{"a", n("1")}})};
    auto parts = ddbsh::render::decompose(ExecOutcome{w});
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(std::get<TextPart>(parts[0]).out, "REPLACE\n");
    EXPECT_EQ(std::get<TextPart>(parts[1]).out, "REPLACE\n");
    EXPECT_EQ(std::get<RowsPart>(parts[2]).rows.size(), 1u);
}

TEST(Decompose, BulkWriteIsTheItemsThenTheAccountingLine) {
    BulkWriteOutcome b;
    b.read = 2;
    b.modified = 2;
    b.returned_items = {row({{"a", n("1")}})};
    auto parts = ddbsh::render::decompose(ExecOutcome{b});
    ASSERT_EQ(parts.size(), 2u);
    EXPECT_TRUE(std::get<RowsPart>(parts[0]).group_end);
    EXPECT_EQ(std::get<TextPart>(parts[1]).out, "UPDATE (2 read, 2 modified, 0 ccf)\n");

    b.partial = true;  // a streaming fragment: one item, no accounting line
    parts = ddbsh::render::decompose(ExecOutcome{b});
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_FALSE(std::get<RowsPart>(parts[0]).group_end);
}

TEST(Decompose, AReportWithATableIsRowsWithFixedColumns) {
    ReportOutcome r;
    r.lines = {"x | y"};
    r.table = ReportTable{{"Z", "A"}, {ValueMap{{"A", s("y")}, {"Z", s("x")}}}, false};
    auto parts = ddbsh::render::decompose(ExecOutcome{r});
    ASSERT_EQ(parts.size(), 1u);
    const auto& rows = std::get<RowsPart>(parts[0]);
    EXPECT_TRUE(rows.fixed_columns);
    EXPECT_FALSE(rows.group_end);
    EXPECT_EQ(rows.columns, (std::vector<std::string>{"Z", "A"}));
    // A report row keeps its column order in JSON.
    EXPECT_EQ(formatted(ExecOutcome{r}, OutputFormat::json),
              "[\n  {\"Z\": \"x\", \"A\": \"y\"}\n]\n");
}

TEST(Decompose, TextArmsStayTextOnBothStreams) {
    StatusOutcome st{"ALTER", {"a diagnostic"}};
    auto parts = ddbsh::render::decompose(ExecOutcome{st});
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(std::get<TextPart>(parts[0]).out, "ALTER\n");
    EXPECT_EQ(std::get<TextPart>(parts[0]).err, "a diagnostic\n");

    TransactOutcome t{"ABORT", {"why"}};
    parts = ddbsh::render::decompose(ExecOutcome{t});
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(std::get<TextPart>(parts[0]).err, "why\n");

    ExplainOutcome e;
    e.blocks = {ExplainBlock{"Scan", "{}", ExplainBlock::Format::pretty}};
    parts = ddbsh::render::decompose(ExecOutcome{e});
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(std::get<TextPart>(parts[0]).out, "Scan({})\n");

    // A report without a table (DESCRIBE) is text.
    parts = ddbsh::render::decompose(ExecOutcome{ReportOutcome{{"Name: t"}}});
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(std::get<TextPart>(parts[0]).out, "Name: t\n");

    // An empty outcome produces no part at all.
    EXPECT_TRUE(ddbsh::render::decompose(ExecOutcome{ddbsh::EmptyOutcome{}}).empty());
}

TEST(Decompose, MultiOutcomeConcatenatesItsParts) {
    MultiOutcome m;
    m.parts = {ExecOutcome{WriteOutcome{{"INSERT"}}}, ExecOutcome{WriteOutcome{{"INSERT"}}}};
    auto parts = ddbsh::render::decompose(ExecOutcome{m});
    ASSERT_EQ(parts.size(), 2u);
    EXPECT_EQ(std::get<TextPart>(parts[1]).out, "INSERT\n");
}

// ---- JSON ---------------------------------------------------------------------

TEST(Json, StringEscapes) {
    std::string out;
    ddbsh::render::append_json_string(out, std::string("q\"b\\\b\f\n\r\t\x01\x1f h\xc3\xa9", 15));
    EXPECT_EQ(out, "\"q\\\"b\\\\\\b\\f\\n\\r\\t\\u0001\\u001f h\xc3\xa9\"");
}

TEST(Json, NumberGrammar) {
    for (const char* ok : {"0", "-0", "7", "-12", "1.5", "0.25", "1e5", "1E+5", "-1.25e-3"}) {
        EXPECT_TRUE(ddbsh::render::is_json_number(ok)) << ok;
    }
    for (const char* bad :
         {"", "-", "01", "1.", ".5", "1e", "1e+", "+1", "Infinity", "NaN", "1.5.5", "1 "}) {
        EXPECT_FALSE(ddbsh::render::is_json_number(bad)) << bad;
    }
    // A number that is not an RFC 8259 number is written as a string, so no
    // digit is lost; an exact long number stays a number, digits unchanged.
    std::string out;
    ddbsh::render::append_json_value(out, n("01"), JsonFlavor::plain);
    EXPECT_EQ(out, "\"01\"");
    out.clear();
    ddbsh::render::append_json_value(out, n("12345678901234567890.123456789"), JsonFlavor::plain);
    EXPECT_EQ(out, "12345678901234567890.123456789");
}

TEST(Json, PlainFlavorForAllTenTypes) {
    SelectOutcome sel;
    sel.items = {all_types()};
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::json),
              "[\n  {\"b\": \"aGk=\", \"bs\": [\"AA==\", \"AQ==\"], \"l\": [1, \"x\"], "
              "\"m\": {\"x\": \"v\", \"y\": 2}, \"n\": 1.5, \"ns\": [1, 2], \"s\": \"str\", "
              "\"ss\": [\"a\", \"b\"], \"t\": true, \"z\": null}\n]\n");
}

TEST(Json, DynamoDbFlavorForAllTenTypes) {
    SelectOutcome sel;
    sel.items = {all_types()};
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::json, JsonFlavor::dynamodb),
              "[\n  {\"b\": {\"B\": \"aGk=\"}, \"bs\": {\"BS\": [\"AA==\", \"AQ==\"]}, "
              "\"l\": {\"L\": [{\"N\": \"1\"}, {\"S\": \"x\"}]}, "
              "\"m\": {\"M\": {\"x\": {\"S\": \"v\"}, \"y\": {\"N\": \"2\"}}}, "
              "\"n\": {\"N\": \"1.5\"}, \"ns\": {\"NS\": [\"1\", \"2\"]}, \"s\": {\"S\": \"str\"}, "
              "\"ss\": {\"SS\": [\"a\", \"b\"]}, \"t\": {\"BOOL\": true}, "
              "\"z\": {\"NULL\": true}}\n]\n");
}

TEST(Json, CellTextForAllTenTypes) {
    const ItemRow item = all_types();
    std::vector<std::string> plain;
    std::vector<std::string> typed;
    for (const auto& [name, value] : item.attributes) {
        plain.push_back(name + "=" + ddbsh::render::format_cell(value, JsonFlavor::plain));
        typed.push_back(name + "=" + ddbsh::render::format_cell(value, JsonFlavor::dynamodb));
    }
    EXPECT_EQ(plain, (std::vector<std::string>{"s=str", "n=1.5", "b=aGk=", "t=true", "z=null",
                                               "l=[1, \"x\"]", "m={\"x\": \"v\", \"y\": 2}",
                                               "ss=[\"a\", \"b\"]", "ns=[1, 2]",
                                               "bs=[\"AA==\", \"AQ==\"]"}));
    // Scalars are raw in either flavor; maps, lists and sets follow it.
    EXPECT_EQ(typed[0], "s=str");
    EXPECT_EQ(typed[3], "t=true");
    EXPECT_EQ(typed[5], "l={\"L\": [{\"N\": \"1\"}, {\"S\": \"x\"}]}");
    EXPECT_EQ(typed[8], "ns={\"NS\": [\"1\", \"2\"]}");
}

TEST(Json, EachElementIsWrittenAsItsRowArrives) {
    Capture c(fmt(OutputFormat::json));
    c.sink.emit(page({row({{"a", n("1")}})}));
    EXPECT_EQ(c.out, "[\n  {\"a\": 1}");  // no waiting for the next page
    c.sink.emit(page({row({{"a", n("2")}})}));
    EXPECT_EQ(c.out, "[\n  {\"a\": 1},\n  {\"a\": 2}");
    c.sink.end_statement();
    EXPECT_EQ(c.out, "[\n  {\"a\": 1},\n  {\"a\": 2}\n]\n");
}

// ---- CSV ----------------------------------------------------------------------

TEST(Csv, QuotesOnlyWhatNeedsQuoting) {
    SelectOutcome sel;
    sel.items = {row({{"a", s("plain")},
                      {"b", s("x,y")},
                      {"c", s("say \"hi\"")},
                      {"d", s("two\nlines")},
                      {"e", s("cr\r")},
                      {"f", s("")}})};
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::csv),
              "a,b,c,d,e,f\nplain,\"x,y\",\"say \"\"hi\"\"\",\"two\nlines\",\"cr\r\",\n");
}

TEST(Csv, HintColumnsComeFirstAndMissingAttributesAreEmpty) {
    SelectOutcome sel;
    sel.column_hint = {"pk", "rk"};
    sel.items = {row({{"z", n("1")}, {"pk", n("1")}, {"rk", n("1")}}),
                 row({{"pk", n("1")}, {"rk", n("2")}})};
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::csv), "pk,rk,z\n1,1,1\n1,2,\n");
}

TEST(Csv, AHeaderIsWrittenAgainWhenColumnsGrow) {
    Capture c(fmt(OutputFormat::csv));
    c.sink.emit(ExecOutcome{SelectOutcome{{row({{"a", n("1")}})}, std::nullopt, {"a"}}});
    c.sink.emit(ExecOutcome{SelectOutcome{{row({{"a", n("2")}})}, std::nullopt, {"a"}}});
    c.sink.emit(
        ExecOutcome{SelectOutcome{{row({{"a", n("3")}, {"b", n("4")}})}, std::nullopt, {"a"}}});
    c.sink.end_statement();
    EXPECT_EQ(c.out, "a\n1\n2\na,b\n3,4\n");
}

TEST(Csv, ColumnTrackerAppendsOnly) {
    ColumnTracker t;
    RowsPart first;
    first.columns = {"pk"};
    first.rows = {row({{"z", n("1")}, {"b", n("1")}, {"pk", n("1")}})};
    EXPECT_EQ(t.add(first), 0u);
    EXPECT_EQ(t.columns(), (std::vector<std::string>{"pk", "b", "z"}));
    RowsPart second;
    second.rows = {row({{"a", n("1")}, {"z", n("2")}})};
    EXPECT_EQ(t.add(second), 3u);
    EXPECT_EQ(t.columns(), (std::vector<std::string>{"pk", "b", "z", "a"}));
    EXPECT_EQ(t.add(second), 4u);  // nothing new
}

// ---- Table --------------------------------------------------------------------

TEST(Table, WidthsCountCodePointsAndNumbersAlignRight) {
    SelectOutcome sel;
    sel.column_hint = {"pk"};
    sel.items = {row({{"pk", n("1")}, {"name", s("h\xc3\xa9llo")}}),
                 row({{"pk", n("100")}, {"name", s("a")}})};
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::table),
              "+-----+-------+\n"
              "| pk  | name  |\n"
              "+-----+-------+\n"
              "|   1 | h\xc3\xa9llo |\n"
              "| 100 | a     |\n"
              "+-----+-------+\n");
}

TEST(Table, ControlCharactersAreEscaped) {
    SelectOutcome sel;
    sel.items = {row({{"a", s(std::string("x\ny\t\x01z", 6))}})};
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::table),
              "+---------------+\n"
              "| a             |\n"
              "+---------------+\n"
              "| x\\ny\\t\\u0001z |\n"
              "+---------------+\n");
}

TEST(Table, OneBoxPerPage) {
    Capture c(fmt(OutputFormat::table));
    c.sink.emit(page({row({{"a", n("1")}})}));
    const std::string first = c.out;
    EXPECT_EQ(first, "+---+\n| a |\n+---+\n| 1 |\n+---+\n");  // printed at the page end
    c.sink.emit(page({row({{"a", n("22")}, {"b", s("x")}})}));
    c.sink.end_statement();
    EXPECT_EQ(c.out, first + "+----+---+\n| a  | b |\n+----+---+\n| 22 | x |\n+----+---+\n");
}

TEST(Table, RowsWaitForTheGroupEndOrTheClose) {
    Capture c(fmt(OutputFormat::table));
    ReportOutcome r1;
    r1.lines = {"a"};
    r1.table = ReportTable{{"TableName"}, {ValueMap{{"TableName", s("a")}}}, false};
    c.sink.emit(ExecOutcome{r1});
    EXPECT_EQ(c.out, "");  // the page has not ended
    // A LIKE dropped the page's last name: an empty fragment ends the page.
    ReportOutcome end;
    end.table = ReportTable{{"TableName"}, {}, true};
    c.sink.emit(ExecOutcome{end});
    EXPECT_EQ(c.out, "+-----------+\n| TableName |\n+-----------+\n| a         |\n+-----------+\n");
    c.sink.end_statement();  // nothing left to print
    EXPECT_EQ(c.out, "+-----------+\n| TableName |\n+-----------+\n| a         |\n+-----------+\n");
}

TEST(Table, ShowTablesOverTwoPagesThroughASession) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"a", "b", "c"});
    client.set_list_tables_page_size(2);
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    Capture c(fmt(OutputFormat::table));
    auto stmt = session.parse("show tables brief like \"[ac]\";");
    ASSERT_TRUE(stmt.has_value());
    ASSERT_TRUE(session.execute(*stmt, c.sink).has_value());
    const std::string box_a =
        "+-----------+\n| TableName |\n+-----------+\n| a         |\n+-----------+\n";
    const std::string box_c =
        "+-----------+\n| TableName |\n+-----------+\n| c         |\n+-----------+\n";
    EXPECT_EQ(c.out, box_a + box_c);
}

// ---- reports ------------------------------------------------------------------

TEST(Buffered, ShowTablesFoldsTheTableRowsOfEveryPage) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"a", "b", "c"});
    client.set_list_tables_page_size(2);
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    auto buffered = session.run("show tables brief;");
    ASSERT_TRUE(buffered.has_value());
    const auto* report = std::get_if<ReportOutcome>(&*buffered);
    ASSERT_NE(report, nullptr);
    EXPECT_EQ(report->lines, (std::vector<std::string>{"a", "b", "c"}));
    ASSERT_TRUE(report->table.has_value());
    EXPECT_EQ(report->table->columns, (std::vector<std::string>{"TableName"}));
    EXPECT_EQ(report->table->rows.size(), 3u);
    EXPECT_EQ(ddbsh::render::render_formatted(*buffered, fmt(OutputFormat::csv)),
              "TableName\na\nb\nc\n");
}

TEST(Reports, ShowBackupsWithNoBackupIsAnEmptyResultSet) {
    ddbsh::test::FakeDDBClient client;
    client.set_backup_pages({{}});
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    Capture c(fmt(OutputFormat::csv));
    auto stmt = session.parse("show backups;");
    ASSERT_TRUE(stmt.has_value());
    ASSERT_TRUE(session.execute(*stmt, c.sink).has_value());
    EXPECT_EQ(c.out,
              "TableName,BackupName,BackupStatus,BackupArn,CreationDateTime,ExpiryDateTime\n");
}

TEST(Reports, ShowBackupsRowsByColumn) {
    ddbsh::test::FakeDDBClient client;
    client.set_backup_pages({{ddbsh::BackupSummaryRow{
        "t", "b1", "AVAILABLE", "arn:b1", "2026-01-01T00:00:00Z", "2026-02-01T00:00:00Z"}}});
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    Capture c(fmt(OutputFormat::json));
    auto stmt = session.parse("show backups;");
    ASSERT_TRUE(stmt.has_value());
    ASSERT_TRUE(session.execute(*stmt, c.sink).has_value());
    EXPECT_EQ(c.out,
              "[\n  {\"TableName\": \"t\", \"BackupName\": \"b1\", \"BackupStatus\": "
              "\"AVAILABLE\", \"BackupArn\": \"arn:b1\", \"CreationDateTime\": "
              "\"2026-01-01T00:00:00Z\", \"ExpiryDateTime\": \"2026-02-01T00:00:00Z\"}\n]\n");
}

TEST(Reports, ShowLimitsIsOneBoxOfFiveRows) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::DescribeLimitsOut limits;
    limits.account_max_rcu = 80000;
    limits.account_max_wcu = 80000;
    limits.table_max_rcu = 40000;
    limits.table_max_wcu = 40000;
    client.set_describe_limits(limits);
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    Capture c(fmt(OutputFormat::table));
    auto stmt = session.parse("show limits;");
    ASSERT_TRUE(stmt.has_value());
    ASSERT_TRUE(session.execute(*stmt, c.sink).has_value());
    EXPECT_EQ(c.out,
              "+-----------+--------------+-------+-------+\n"
              "| Region    | Limit        | RCU   | WCU   |\n"
              "+-----------+--------------+-------+-------+\n"
              "| us-east-1 | Account Max  | 80000 | 80000 |\n"
              "| us-east-1 | Table Max    | 40000 | 40000 |\n"
              "| us-east-1 | Total Tables |     0 |     0 |\n"
              "| us-east-1 | Total GSI    |     0 |     0 |\n"
              "| us-east-1 | Total        |     0 |     0 |\n"
              "+-----------+--------------+-------+-------+\n");
}

// ---- empty results ------------------------------------------------------------

TEST(Empty, EveryFormatShowsAnEmptyResultSet) {
    SelectOutcome sel;
    sel.column_hint = {"pk", "rk"};
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::json), "[]\n");
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::csv), "pk,rk\n");
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::table),
              "+----+----+\n| pk | rk |\n+----+----+\n");
    EXPECT_EQ(formatted(ExecOutcome{sel}, OutputFormat::standard), "");
    // With no known column there is nothing to draw, and no header.
    EXPECT_EQ(formatted(ExecOutcome{SelectOutcome{}}, OutputFormat::csv), "");
    EXPECT_EQ(formatted(ExecOutcome{SelectOutcome{}}, OutputFormat::table), "");
}

TEST(Empty, AnEmptySelectThroughASessionIsAnEmptyArray) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::DescribeTableOut t;
    t.table_name = "t";
    t.key_schema = ddbsh::TableKeySchema{{"pk"}, {"rk"}};
    client.set_table(t);
    client.set_pages({{}});
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    for (auto [format, expected] : {std::pair{OutputFormat::json, std::string("[]\n")},
                                    std::pair{OutputFormat::csv, std::string("pk,rk\n")}}) {
        Capture c(fmt(format));
        auto stmt = session.parse("select * from t;");
        ASSERT_TRUE(stmt.has_value());
        ASSERT_TRUE(session.execute(*stmt, c.sink).has_value());
        EXPECT_EQ(c.out, expected);
    }
}

// ---- result sets and text -----------------------------------------------------

TEST(ResultSet, TextClosesTheOpenSetFirst) {
    Capture c(fmt(OutputFormat::json));
    c.sink.emit(page({row({{"a", n("1")}})}));
    c.sink.emit(ExecOutcome{StatusOutcome{"CONNECT"}});
    c.sink.emit(page({row({{"a", n("2")}})}));
    c.sink.end_statement();
    EXPECT_EQ(c.out, "[\n  {\"a\": 1}\n]\nCONNECT\n[\n  {\"a\": 2}\n]\n");
}

TEST(ResultSet, AFailureOnPageTwoClosesTheSetBeforeTheError) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::DescribeTableOut t;
    t.table_name = "t";
    t.key_schema = ddbsh::TableKeySchema{{"a"}, {}};
    client.set_table(t);
    client.set_pages({{row({{"a", n("1")}})}, {row({{"a", n("2")}})}});
    client.set_read_error_at_page(
        1, ddbsh::Error{ddbsh::Errc::service_error, "throttled out", std::nullopt, std::nullopt});
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    ddbsh::render::Renderer renderer;
    Capture c(fmt(OutputFormat::json));
    ddbsh::cli::run_batch(session, renderer, "select * from t; ", false, c.sink,
                          [&c](const std::string& e) { c.log += "E:" + e; });
    EXPECT_EQ(c.log, "O:[\n  {\"a\": 1}O:\n]\nE:throttled out\n");
}

TEST(ResultSet, SetOptionsClosesTheOpenSetInTheOldFormat) {
    Capture c(fmt(OutputFormat::json));
    c.sink.emit(page({row({{"a", n("1")}})}));
    c.sink.set_options(fmt(OutputFormat::csv));
    EXPECT_EQ(c.out, "[\n  {\"a\": 1}\n]\n");
    EXPECT_EQ(c.sink.options().format, OutputFormat::csv);
}

// ---- default == Renderer ------------------------------------------------------

TEST(DefaultFormat, FormattingSinkWritesExactlyTheRenderer) {
    ddbsh::ConsumedCapacity cc;
    cc.table_name = "t";
    cc.capacity_units = 1.0;
    ReportOutcome table_report{{"a | b"}, ReportTable{{"A"}, {ValueMap{{"A", s("a")}}}, true}};
    BulkWriteOutcome bulk;
    bulk.returned_items = {row({{"a", n("1")}})};
    VectorSearchOutcome vec;
    vec.rows = {{row({{"a", n("1")}}), 0.5}};
    ExplainOutcome explain;
    explain.blocks = {
        ExplainBlock{"GetItem", "{\n\t\"k\":\t\"v\"\n}", ExplainBlock::Format::pretty}};
    WriteOutcome write;
    write.tokens = {"REPLACE"};
    write.items = {row({{"a", n("1")}})};
    const std::vector<ExecOutcome> arms{
        ExecOutcome{StatusOutcome{"ALTER", {"diag"}}},
        ExecOutcome{ReportOutcome{{"x", "y"}}},
        ExecOutcome{table_report},
        ExecOutcome{ddbsh::EmptyOutcome{}},
        ExecOutcome{SelectOutcome{{all_types()}, cc, {"s"}}},
        ExecOutcome{write},
        ExecOutcome{explain},
        ExecOutcome{bulk},
        ExecOutcome{TransactOutcome{"ABORT", {"why"}}},
        ExecOutcome{TransactReadOutcome{{row({{"a", n("1")}}), ItemRow{}}}},
        ExecOutcome{vec},
        ExecOutcome{MultiOutcome{{ExecOutcome{StatusOutcome{"A"}}, ExecOutcome{bulk}}}},
    };
    const ddbsh::render::Renderer renderer;
    for (std::size_t i = 0; i < arms.size(); ++i) {
        Capture c(fmt(OutputFormat::standard));
        c.sink.emit(arms[i]);
        c.sink.end_statement();
        EXPECT_EQ(c.out, renderer.render(arms[i])) << "arm " << i;
        EXPECT_EQ(c.err, renderer.render_stderr(arms[i])) << "arm " << i;
        EXPECT_EQ(ddbsh::render::render_formatted(arms[i], fmt(OutputFormat::standard)),
                  renderer.render(arms[i]))
            << "arm " << i;
    }
}

// ---- the CLI's directives ------------------------------------------------------

TEST(CliOutputDirective, RecognizesAndAppliesTheTwoDirectives) {
    FormatOptions o;
    std::string error;
    EXPECT_TRUE(ddbsh::cli::try_set_output_directive("set output json", o, error));
    EXPECT_EQ(o.format, OutputFormat::json);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(ddbsh::cli::try_set_output_directive("SET JSON-FLAVOR \"dynamodb\"", o, error));
    EXPECT_EQ(o.json_flavor, JsonFlavor::dynamodb);
    EXPECT_TRUE(ddbsh::cli::try_set_output_directive("set  output\tdefault", o, error));
    EXPECT_EQ(o.format, OutputFormat::standard);
    EXPECT_FALSE(ddbsh::cli::try_set_output_directive("set terminator go", o, error));
    EXPECT_FALSE(ddbsh::cli::try_set_output_directive("select output from t;", o, error));
}

TEST(CliOutputDirective, AnUnknownValueChangesNothingAndSaysWhy) {
    FormatOptions o{OutputFormat::csv, JsonFlavor::plain};
    std::string error;
    EXPECT_TRUE(ddbsh::cli::try_set_output_directive("set output xml", o, error));
    EXPECT_EQ(o.format, OutputFormat::csv);
    EXPECT_EQ(error, "unknown output format: xml (expected default, json, csv, table)\n");
    EXPECT_TRUE(ddbsh::cli::try_set_output_directive("set json-flavor", o, error));
    EXPECT_EQ(error, "unknown JSON flavor:  (expected plain, dynamodb)\n");
}

TEST(CliOutputDirective, RunSourceAppliesADirectiveAtBatchStart) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    ddbsh::render::Renderer renderer;
    Capture c(fmt(OutputFormat::standard));
    std::istringstream in("set output json\nconnect us-east-2;\ngo\nset output xml\nquit;\n");
    ddbsh::cli::StreamLineSource src(in);
    ddbsh::cli::Terminator term;
    const int rc = ddbsh::cli::run_source(session, renderer, src, term, false, c.sink,
                                          [&c](const std::string& e) { c.log += "E:" + e; });
    EXPECT_EQ(rc, 0);  // an unknown value does not change the exit code
    EXPECT_EQ(c.sink.options().format, OutputFormat::json);
    EXPECT_EQ(c.log,
              "O:CONNECT\nE:unknown output format: xml (expected default, json, csv, table)\n"
              "O:QUIT\n");
}

TEST(CliOutputDirective, TheFdOverloadCarriesTheSelectionInAndOut) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, client);
    ddbsh::render::Renderer renderer;
    // As main() does with ~/.ddbsh_config: a config's directive comes back
    // out, a flag then overrides one field, and the session starts from that.
    FormatOptions format;
    std::istringstream cfg("set output csv\nset json-flavor dynamodb\n");
    ddbsh::cli::StreamLineSource cfg_src(cfg);
    ddbsh::cli::Terminator term;
    EXPECT_EQ(ddbsh::cli::run_source(session, renderer, cfg_src, term, true, &format), 0);
    EXPECT_EQ(format.format, OutputFormat::csv);
    EXPECT_EQ(format.json_flavor, JsonFlavor::dynamodb);
}

}  // namespace
