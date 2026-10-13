// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_exec_test.cpp — W5 SELECT execution + rendering unit tests.
//
// Exercises the full non-EXPLAIN SELECT path offline via FakeDDBClient:
//   * GetItem / Query / Scan dispatch (the planner's §8 decision drives which).
//   * Query/Scan pagination (the executor follows LastEvaluatedKey).
//   * Consumed-capacity totalizing across pages and the display gating on the
//     user's RETURN CONSUMED CAPACITY (a ratelimit alone shows nothing).
//   * Byte-exact item rendering (arch §9): alpha-sorted attributes, the
//     top-level `: ` vs nested `:` asymmetry, quote_if_required, TRUE/FALSE/
//     NULL, lists/maps/empties, and the `SELECT (table, cu, rcu, wcu)` line.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::ConsumedCapacity;
using ddbsh::DescribeTableOut;
using ddbsh::ItemRow;
using ddbsh::TableKeySchema;
using ddbsh::Value;
using ddbsh::ValueMap;

DescribeTableOut table_a_b(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{"a", "b"};
    return t;
}

DescribeTableOut table_a(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{"a", ""};
    return t;
}

ItemRow row(ValueMap attrs) {
    return ItemRow{std::move(attrs)};
}

Value num(const std::string& n) {
    return Value::number_value(n);
}
Value str(const std::string& s) {
    return Value::string_value(s);
}

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

// ---- GetItem path ----------------------------------------------------------

TEST(SelectExec, GetItemReturnsRenderedItem) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    client.set_get_item(row({{"a", num("2")}, {"c", num("9")}}));
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 2;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "{a: 2, c: 9}\n");
}

TEST(SelectExec, GetItemNotFoundRendersNothing) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    client.set_get_item(std::nullopt);
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 2;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "");
}

// ---- Query path + attribute sorting ----------------------------------------

TEST(SelectExec, QueryRendersItemsAlphaSorted) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    // Attributes supplied out of order; the renderer sorts them.
    client.set_items({row({{"pk", num("9")}, {"a", num("0")}, {"b", num("1")}})});
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 2;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "{a: 0, b: 1, pk: 9}\n");
}

TEST(SelectExec, QueryPaginationAggregatesAllPages) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    client.set_pages(
        {{row({{"a", num("1")}})}, {row({{"a", num("2")}})}, {row({{"a", num("3")}})}});
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 2;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "{a: 1}\n{a: 2}\n{a: 3}\n");
    EXPECT_EQ(client.read_calls(), 3);  // one call per page
}

// ---- Scan path -------------------------------------------------------------

TEST(SelectExec, ScanRendersItems) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    client.set_items({row({{"a", num("4")}, {"c", num("6")}})});
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t;");  // no WHERE => scan
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "{a: 4, c: 6}\n");
}

// ---- Consumed capacity -----------------------------------------------------

TEST(SelectExec, ReturnConsumedCapacityShowsSelectLine) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    client.set_items({row({{"a", num("2")}})});
    ConsumedCapacity cc;
    cc.table_name = "t";
    cc.capacity_units = 0.5;
    client.set_consumed_capacity(cc);
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 2 return consumed capacity total;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "{a: 2}\nSELECT (t, 0.5, 0, 0)\n");
}

TEST(SelectExec, ConsumedCapacityTotalizedAcrossPages) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    client.set_pages({{row({{"a", num("1")}})}, {row({{"a", num("2")}})}});
    ConsumedCapacity cc;
    cc.table_name = "t";
    cc.capacity_units = 1.0;  // per page => 2.0 total over two pages
    client.set_consumed_capacity(cc);
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 2 return consumed capacity total;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "{a: 1}\n{a: 2}\nSELECT (t, 2, 0, 0)\n");
}

TEST(SelectExec, RatelimitAloneDoesNotShowConsumedCapacity) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    client.set_items({row({{"a", num("2")}})});
    ConsumedCapacity cc;
    cc.table_name = "t";
    cc.capacity_units = 3.0;
    client.set_consumed_capacity(cc);
    ddbsh::Session session = make_session(client);

    // Ratelimit forces the request to TOTAL, but the display is gated on the
    // user's RETURN CONSUMED CAPACITY (none here), so no SELECT line.
    auto out = session.run("select * from t with ratelimit ( 10 rcu, 20 wcu );");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "{a: 2}\n");
}

// ---- Value rendering specifics (arch §9) -----------------------------------

TEST(SelectExec, StringQuotingAndScalarTypes) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    client.set_get_item(row({
        {"a", num("1")},
        {"plain", str("hello")},
        {"spaced", str("hello world")},  // space => quoted
        {"flag", Value::bool_value(true)},
        {"off", Value::bool_value(false)},
        {"nothing", Value::null_value()},
    }));
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 1;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out),
              "{a: 1, flag: TRUE, nothing: NULL, off: FALSE, plain: hello, "
              "spaced: \"hello world\"}\n");
}

TEST(SelectExec, NestedListAndMapFormatting) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    Value list;
    list.type = Value::Type::L;
    list.list = {num("3"), num("4"), str("hello")};
    Value map;
    map.type = Value::Type::M;
    // Supplied out of order; nested map keys are sorted and use `:` (no space).
    map.map = {{"c", num("36")}, {"a", num("36")}, {"b", num("36")}};
    client.set_get_item(row({{"a", num("1")}, {"lst", list}, {"mp", map}}));
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 1;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "{a: 1, lst: [3, 4, hello], mp: {a:36, b:36, c:36}}\n");
}

TEST(SelectExec, EmptyListAndMap) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    Value empty_list;
    empty_list.type = Value::Type::L;
    Value empty_map;
    empty_map.type = Value::Type::M;
    client.set_get_item(row({{"a", num("1")}, {"l", empty_list}, {"m", empty_map}}));
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t where a = 1;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "{a: 1, l: [], m: {}}\n");
}

// ---- Error path ------------------------------------------------------------

TEST(SelectExec, MissingTablePropagatesError) {
    ddbsh::test::FakeDDBClient client;  // no tables registered
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from nope where a = 2;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ddbsh::Errc::no_such_table);
}

TEST(SelectExec, ExplainSelectReportsNotImplemented) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("explain select * from t where a = 1;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ddbsh::Errc::not_implemented);
}

}  // namespace
