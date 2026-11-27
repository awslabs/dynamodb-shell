// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// planner_test.cpp — W4 SELECT planner unit tests (no live backend).
//
// Pins the GetItem/Query/Scan decision matrix (arch §8) and the exact request
// each decision builds — the aliased ProjectionExpression /
// KeyConditionExpression / FilterExpression strings and the
// ExpressionAttributeNames / ExpressionAttributeValues maps — against a fixed
// alias prefix ("aaa", the single-statement prefix the goldens use). Several
// expectations are copied verbatim from the oracle .exp goldens (isel-1,
// isel-107, isel-137, isel-189) so this test is a direct fidelity check of the
// §6 alias scheme and §8 planner, fully offline via FakeDDBClient-served
// schemas.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "ddbsh/session.hpp"
#include "ddbsh/statement.hpp"

#include "fake_ddb_client.hpp"
#include "parser/ast.hpp"
#include "planner/select_planner.hpp"

namespace {

using ddbsh::DescribeTableOut;
using ddbsh::ReturnConsumedCapacity;
using ddbsh::SecondaryIndexSchema;
using ddbsh::TableKeySchema;
using ddbsh::engine::plan_select;
using ddbsh::engine::SelectApi;
using ddbsh::engine::SelectCmd;
using ddbsh::engine::SelectPlan;

// A table with partition key `a` and sort key `b`.
DescribeTableOut table_a_b(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{{"a"}, {"b"}};
    return t;
}

// A hash-only table with partition key `a`.
DescribeTableOut table_a(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{{"a"}, {}};
    return t;
}

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

// Parse `text`, require a SELECT, and plan it against `client` with the
// single-statement alias prefix "aaa".
SelectPlan plan_ok(ddbsh::Session& session, ddbsh::test::FakeDDBClient& client,
                   const std::string& text) {
    auto stmt = session.parse(text);
    EXPECT_TRUE(stmt.has_value()) << "parse failed: "
                                  << (stmt.has_value() ? std::string{} : stmt.error().message);
    const auto& sel = std::get<SelectCmd>(stmt->ast()->command);
    auto plan = plan_select(sel, client, "aaa");
    EXPECT_TRUE(plan.has_value()) << "plan failed: "
                                  << (plan.has_value() ? std::string{} : plan.error().message);
    return std::move(plan.value());
}

// ---- GetItem decisions -----------------------------------------------------

TEST(Planner, FullHashRangeKeyIsGetItem) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t where a = 2 and b = 3;");
    ASSERT_EQ(plan.api, SelectApi::get_item);
    const auto& r = plan.get_item;
    EXPECT_EQ(r.table_name, "t");
    EXPECT_FALSE(r.consistent_read);
    EXPECT_FALSE(r.projection_expression.has_value());  // select *
    EXPECT_TRUE(r.expression_attribute_names.empty());  // no aliases without projection
    EXPECT_EQ(r.return_consumed_capacity, ReturnConsumedCapacity::none);

    // Key map is the literal (non-aliased) full primary key, sorted by name.
    ASSERT_EQ(r.key.size(), 2u);
    EXPECT_EQ(r.key[0].first, "a");
    EXPECT_EQ(r.key[0].second.type, ddbsh::Value::Type::N);
    EXPECT_EQ(r.key[0].second.s, "2");
    EXPECT_EQ(r.key[1].first, "b");
    EXPECT_EQ(r.key[1].second.s, "3");
}

TEST(Planner, HashOnlyFullKeyIsGetItem) {
    // Matches the shape of oracle isel-189 (GetItem on a hash-only table).
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("iselN"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from iselN where a = 2;");
    ASSERT_EQ(plan.api, SelectApi::get_item);
    const auto& r = plan.get_item;
    EXPECT_EQ(r.table_name, "iselN");
    ASSERT_EQ(r.key.size(), 1u);
    EXPECT_EQ(r.key[0].first, "a");
    EXPECT_EQ(r.key[0].second.s, "2");
    EXPECT_FALSE(r.consistent_read);
    EXPECT_EQ(r.return_consumed_capacity, ReturnConsumedCapacity::none);
}

TEST(Planner, GetItemWithProjectionAliasesAttributes) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select a, c from t where a = 2;");
    ASSERT_EQ(plan.api, SelectApi::get_item);
    const auto& r = plan.get_item;
    ASSERT_TRUE(r.projection_expression.has_value());
    EXPECT_EQ(*r.projection_expression, "#aaaa1, #aaaa2");
    ASSERT_EQ(r.expression_attribute_names.size(), 2u);
    EXPECT_EQ(r.expression_attribute_names[0].first, "#aaaa1");
    EXPECT_EQ(r.expression_attribute_names[0].second, "a");
    EXPECT_EQ(r.expression_attribute_names[1].first, "#aaaa2");
    EXPECT_EQ(r.expression_attribute_names[1].second, "c");
}

TEST(Planner, ConsistentReadFlows) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select consistent * from t where a = 2;");
    ASSERT_EQ(plan.api, SelectApi::get_item);
    EXPECT_TRUE(plan.get_item.consistent_read);
}

// ---- Query decisions -------------------------------------------------------

TEST(Planner, HashEqualityOnlyIsQuery) {
    // Table has a sort key, so a bare hash `=` cannot be a GetItem; it queries.
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t where a = 2;");
    ASSERT_EQ(plan.api, SelectApi::query);
    const auto& r = plan.query;
    EXPECT_EQ(r.table_name, "t");
    EXPECT_FALSE(r.index_name.has_value());
    EXPECT_EQ(r.key_condition_expression, "#aaaa1 = :vaaa1");
    EXPECT_FALSE(r.filter_expression.has_value());
    ASSERT_EQ(r.expression_attribute_names.size(), 1u);
    EXPECT_EQ(r.expression_attribute_names[0].first, "#aaaa1");
    EXPECT_EQ(r.expression_attribute_names[0].second, "a");
    ASSERT_EQ(r.expression_attribute_values.size(), 1u);
    EXPECT_EQ(r.expression_attribute_values[0].first, ":vaaa1");
    EXPECT_EQ(r.expression_attribute_values[0].second.s, "2");
    EXPECT_EQ(r.return_consumed_capacity, ReturnConsumedCapacity::none);
}

TEST(Planner, HashEqualityPlusRangeBetweenIsQueryKeyCondition) {
    // Verbatim contract from oracle isel-107.exp.
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("isel1"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select * from isel1 where a = 2 and b between 3 and 5;");
    ASSERT_EQ(plan.api, SelectApi::query);
    const auto& r = plan.query;
    EXPECT_EQ(r.table_name, "isel1");
    EXPECT_EQ(r.key_condition_expression, "#aaaa1 = :vaaa1 AND #aaaa2 BETWEEN :vaaa2 AND :vaaa3");
    EXPECT_FALSE(r.filter_expression.has_value());
    ASSERT_EQ(r.expression_attribute_names.size(), 2u);
    EXPECT_EQ(r.expression_attribute_names[0].second, "a");
    EXPECT_EQ(r.expression_attribute_names[1].second, "b");
    ASSERT_EQ(r.expression_attribute_values.size(), 3u);
    EXPECT_EQ(r.expression_attribute_values[0].first, ":vaaa1");
    EXPECT_EQ(r.expression_attribute_values[0].second.s, "2");
    EXPECT_EQ(r.expression_attribute_values[1].second.s, "3");
    EXPECT_EQ(r.expression_attribute_values[2].second.s, "5");
}

TEST(Planner, HashEqualityPlusNonKeyPredicateIsQueryWithFilter) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t where a = 2 and c = 9;");
    ASSERT_EQ(plan.api, SelectApi::query);
    const auto& r = plan.query;
    EXPECT_EQ(r.key_condition_expression, "#aaaa1 = :vaaa1");
    ASSERT_TRUE(r.filter_expression.has_value());
    EXPECT_EQ(*r.filter_expression, "#aaaa2 = :vaaa2");
    // a (key cond) then c (filter): names aliased in call order.
    ASSERT_EQ(r.expression_attribute_names.size(), 2u);
    EXPECT_EQ(r.expression_attribute_names[0].second, "a");
    EXPECT_EQ(r.expression_attribute_names[1].second, "c");
}

// ---- Scan decisions --------------------------------------------------------

TEST(Planner, NoWhereIsScan) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t;");
    ASSERT_EQ(plan.api, SelectApi::scan);
    const auto& r = plan.scan;
    EXPECT_EQ(r.table_name, "t");
    EXPECT_FALSE(r.filter_expression.has_value());
    EXPECT_TRUE(r.expression_attribute_names.empty());
    EXPECT_TRUE(r.expression_attribute_values.empty());
}

TEST(Planner, AttributeExistsIsScanWithFilter) {
    // Verbatim contract from oracle isel-1.exp.
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("isel1"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from isel1 where attribute_exists(c);");
    ASSERT_EQ(plan.api, SelectApi::scan);
    const auto& r = plan.scan;
    EXPECT_EQ(r.table_name, "isel1");
    ASSERT_TRUE(r.filter_expression.has_value());
    EXPECT_EQ(*r.filter_expression, "attribute_exists(#aaaa1)");
    ASSERT_EQ(r.expression_attribute_names.size(), 1u);
    EXPECT_EQ(r.expression_attribute_names[0].first, "#aaaa1");
    EXPECT_EQ(r.expression_attribute_names[0].second, "c");
    EXPECT_TRUE(r.expression_attribute_values.empty());
}

TEST(Planner, ScanWithProjectionAndFilter) {
    // Verbatim contract from oracle isel-137.exp: projection aliased first
    // (a->#aaaa1, ex->#aaaa2), filter reuses the ex token.
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("isel1"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select a, ex from isel1 where attribute_exists(ex);");
    ASSERT_EQ(plan.api, SelectApi::scan);
    const auto& r = plan.scan;
    ASSERT_TRUE(r.projection_expression.has_value());
    EXPECT_EQ(*r.projection_expression, "#aaaa1, #aaaa2");
    ASSERT_TRUE(r.filter_expression.has_value());
    EXPECT_EQ(*r.filter_expression, "attribute_exists(#aaaa2)");
    ASSERT_EQ(r.expression_attribute_names.size(), 2u);
    EXPECT_EQ(r.expression_attribute_names[0].second, "a");
    EXPECT_EQ(r.expression_attribute_names[1].second, "ex");
}

TEST(Planner, HashInequalityIsScan) {
    // A `!=` on the hash key is illegal in a KeyConditionExpression, so the
    // whole predicate becomes a scan filter (desugared NOT (#a = :v), arch §8).
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t where a != 2;");
    ASSERT_EQ(plan.api, SelectApi::scan);
    const auto& r = plan.scan;
    ASSERT_TRUE(r.filter_expression.has_value());
    EXPECT_EQ(*r.filter_expression, "NOT (#aaaa1 = :vaaa1)");
}

// ---- Index targeting -------------------------------------------------------

TEST(Planner, IndexHashEqualityIsQueryWithIndexName) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t = table_a_b("t");
    t.global_secondary_indexes.push_back(
        SecondaryIndexSchema{.index_name = "myindex", .key_schema = TableKeySchema{{"g"}, {}}});
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t.myindex where g = 7;");
    ASSERT_EQ(plan.api, SelectApi::query);
    const auto& r = plan.query;
    ASSERT_TRUE(r.index_name.has_value());
    EXPECT_EQ(*r.index_name, "myindex");
    EXPECT_EQ(r.key_condition_expression, "#aaaa1 = :vaaa1");
    EXPECT_EQ(r.expression_attribute_names[0].second, "g");
}

TEST(Planner, IndexNonKeyPredicateIsScanWithIndexName) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t = table_a_b("t");
    t.local_secondary_indexes.push_back(
        SecondaryIndexSchema{.index_name = "myidx", .key_schema = TableKeySchema{{"a"}, {"l"}}});
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t.myidx where attribute_exists(c);");
    ASSERT_EQ(plan.api, SelectApi::scan);
    const auto& r = plan.scan;
    ASSERT_TRUE(r.index_name.has_value());
    EXPECT_EQ(*r.index_name, "myidx");
    ASSERT_TRUE(r.filter_expression.has_value());
    EXPECT_EQ(*r.filter_expression, "attribute_exists(#aaaa1)");
}

// ---- Composite (multipart) GSI keys (phase 4a) -----------------------------

// A base table (pk `a`, sk `b`) carrying a GSI `g` with a composite partition
// key (a, b) and a composite sort key (c, d) — the "multi-PK + multi-SK" shape.
DescribeTableOut table_composite_gsi(const std::string& name) {
    DescribeTableOut t = table_a_b(name);
    SecondaryIndexSchema g;
    g.index_name = "g";
    g.key_schema.hash_keys = {"a", "b"};
    g.key_schema.range_keys = {"c", "d"};
    t.global_secondary_indexes.push_back(std::move(g));
    return t;
}

TEST(Planner, CompositeGsiFullPkNoSkIsQuery) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_composite_gsi("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t.g where a = 1 and b = 2;");
    ASSERT_EQ(plan.api, SelectApi::query);
    const auto& r = plan.query;
    ASSERT_TRUE(r.index_name.has_value());
    EXPECT_EQ(*r.index_name, "g");
    EXPECT_EQ(r.key_condition_expression, "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2");
    EXPECT_FALSE(r.filter_expression.has_value());
}

TEST(Planner, CompositeGsiFullPkFullSkPrefixRangeOnLastIsQuery) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_composite_gsi("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select * from t.g where a = 1 and b = 2 and c = 3 and d > 4;");
    ASSERT_EQ(plan.api, SelectApi::query);
    const auto& r = plan.query;
    EXPECT_EQ(r.key_condition_expression,
              "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2 AND #aaaa3 = :vaaa3 AND #aaaa4 > :vaaa4");
    EXPECT_FALSE(r.filter_expression.has_value());
}

TEST(Planner, CompositeGsiPartialSkPrefixIsQuery) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_composite_gsi("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select * from t.g where a = 1 and b = 2 and c = 3;");
    ASSERT_EQ(plan.api, SelectApi::query);
    const auto& r = plan.query;
    EXPECT_EQ(r.key_condition_expression,
              "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2 AND #aaaa3 = :vaaa3");
    EXPECT_FALSE(r.filter_expression.has_value());
}

TEST(Planner, CompositeGsiPartialPkIsScan) {
    // `b` (a partition-key attr) missing -> plan_query fails -> Scan, whole
    // predicate becomes the FilterExpression.
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_composite_gsi("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan = plan_ok(session, client, "select * from t.g where a = 1 and c = 3;");
    ASSERT_EQ(plan.api, SelectApi::scan);
    const auto& r = plan.scan;
    ASSERT_TRUE(r.index_name.has_value());
    EXPECT_EQ(*r.index_name, "g");
    ASSERT_TRUE(r.filter_expression.has_value());
    EXPECT_EQ(*r.filter_expression, "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2");
}

TEST(Planner, CompositeGsiSkGapIsScan) {
    // `c` (sk1) absent but `d` (sk2) present -> gap -> Scan.
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_composite_gsi("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select * from t.g where a = 1 and b = 2 and d = 4;");
    ASSERT_EQ(plan.api, SelectApi::scan);
    EXPECT_TRUE(plan.scan.filter_expression.has_value());
}

TEST(Planner, CompositeGsiRangeOnNonLastSkIsScan) {
    // `c` is a non-last prefix element carrying a range op -> Scan.
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_composite_gsi("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select * from t.g where a = 1 and b = 2 and c > 3 and d = 4;");
    ASSERT_EQ(plan.api, SelectApi::scan);
    EXPECT_TRUE(plan.scan.filter_expression.has_value());
}

TEST(Planner, CompositeGsiMultiPkOnlyIsQuery) {
    // A GSI whose key is a composite partition key (a, b, c) and no sort key.
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t = table_a_b("t");
    SecondaryIndexSchema g;
    g.index_name = "g";
    g.key_schema.hash_keys = {"a", "b", "c"};
    t.global_secondary_indexes.push_back(std::move(g));
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select * from t.g where a = 1 and b = 2 and c = 3;");
    ASSERT_EQ(plan.api, SelectApi::query);
    EXPECT_EQ(plan.query.key_condition_expression,
              "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2 AND #aaaa3 = :vaaa3");
}

// ---- ReturnConsumedCapacity / ratelimit ------------------------------------

TEST(Planner, ReturnConsumedCapacityTotalFlows) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select * from t where a = 2 return consumed capacity total;");
    ASSERT_EQ(plan.api, SelectApi::query);
    EXPECT_EQ(plan.query.return_consumed_capacity, ReturnConsumedCapacity::total);
}

TEST(Planner, RatelimitForcesTotalOnScan) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    ddbsh::Session session = make_session(client);

    SelectPlan plan =
        plan_ok(session, client, "select * from t with ratelimit ( 10 rcu, 20 wcu );");
    ASSERT_EQ(plan.api, SelectApi::scan);
    EXPECT_EQ(plan.scan.return_consumed_capacity, ReturnConsumedCapacity::total);
}

// ---- Error path ------------------------------------------------------------

TEST(Planner, MissingTablePropagatesError) {
    ddbsh::test::FakeDDBClient client;  // no tables registered
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("select * from nope where a = 2;");
    ASSERT_TRUE(stmt.has_value());
    const auto& sel = std::get<SelectCmd>(stmt->ast()->command);
    auto plan = plan_select(sel, client, "aaa");
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, ddbsh::Errc::no_such_table);
}

TEST(Planner, MissingIndexIsNoSuchTable) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));  // no such index "missing"
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("select * from t.missing where a = 2;");
    ASSERT_TRUE(stmt.has_value());
    const auto& sel = std::get<SelectCmd>(stmt->ast()->command);
    auto plan = plan_select(sel, client, "aaa");
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().code, ddbsh::Errc::no_such_table);
}

// ---- not-equal on a sort key (P15, B6, D-P15-8) --------------------------------
//
// `<>`, `!=` and `NOT x = v` are one operator. With the hash key `=` and a
// single not-equal on the (last) sort key, the plan is two Queries: `< v`, then
// `> v`, otherwise identical. The tables mirror the `ine-*` golden family's
// seed (tests/golden/families/not-equal.txt).

// ddbsh_ne: (pk N HASH, sk N RANGE), LSI ne_lsi (pk, l), GSI ne_gsi (g, h),
// composite GSI ne_cgsi ((g, q) HASH, (h, r) RANGE); declared types as the
// real table reports them.
DescribeTableOut ne_table() {
    DescribeTableOut t;
    t.table_name = "ddbsh_ne";
    t.key_schema = TableKeySchema{{"pk"}, {"sk"}};
    t.local_secondary_indexes.push_back(
        SecondaryIndexSchema{.index_name = "ne_lsi", .key_schema = TableKeySchema{{"pk"}, {"l"}}});
    t.global_secondary_indexes.push_back(
        SecondaryIndexSchema{.index_name = "ne_gsi", .key_schema = TableKeySchema{{"g"}, {"h"}}});
    t.global_secondary_indexes.push_back(SecondaryIndexSchema{
        .index_name = "ne_cgsi", .key_schema = TableKeySchema{{"g", "q"}, {"h", "r"}}});
    using ddbsh::AttributeDef;
    using ddbsh::ScalarType;
    t.attribute_definitions = {AttributeDef{"pk", ScalarType::n}, AttributeDef{"sk", ScalarType::n},
                               AttributeDef{"l", ScalarType::n},  AttributeDef{"g", ScalarType::s},
                               AttributeDef{"h", ScalarType::n},  AttributeDef{"q", ScalarType::s},
                               AttributeDef{"r", ScalarType::n}};
    return t;
}

// The two legs differ only in the key condition.
void expect_legs_match(const ddbsh::QueryIn& lower, const ddbsh::QueryIn& upper) {
    EXPECT_EQ(lower.table_name, upper.table_name);
    EXPECT_EQ(lower.index_name, upper.index_name);
    EXPECT_EQ(lower.filter_expression, upper.filter_expression);
    EXPECT_EQ(lower.projection_expression, upper.projection_expression);
    EXPECT_EQ(lower.expression_attribute_names, upper.expression_attribute_names);
    ASSERT_EQ(lower.expression_attribute_values.size(), upper.expression_attribute_values.size());
    for (std::size_t i = 0; i < lower.expression_attribute_values.size(); ++i) {
        EXPECT_EQ(lower.expression_attribute_values[i].first,
                  upper.expression_attribute_values[i].first);
        EXPECT_EQ(lower.expression_attribute_values[i].second.s,
                  upper.expression_attribute_values[i].second.s);
    }
    EXPECT_EQ(lower.consistent_read, upper.consistent_read);
    EXPECT_EQ(lower.return_consumed_capacity, upper.return_consumed_capacity);
    EXPECT_FALSE(upper.exclusive_start_key.has_value());
}

struct NeCase {
    const char* sql;
    SelectApi api;
    bool split;
    const char* lower_kc;  // the (lower) key condition, when api == query
};

TEST(PlannerNotEqual, SplitAndNoSplitTable) {
    const NeCase cases[] = {
        {"select * from ddbsh_ne where pk = 1 and sk <> 5;", SelectApi::query, true,
         "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2"},
        {"select * from ddbsh_ne where pk = 1 and sk != 5;", SelectApi::query, true,
         "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2"},
        {"select * from ddbsh_ne where pk = 1 and not sk = 5;", SelectApi::query, true,
         "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2"},
        {"select * from ddbsh_ne where sk <> 5 and pk = 1;", SelectApi::query, true,
         "#aaaa1 < :vaaa1 AND #aaaa2 = :vaaa2"},
        {"select * from ddbsh_ne where pk = 1 and sk <> 5 and x = 3;", SelectApi::query, true,
         "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2"},
        {"select * from ddbsh_ne where pk = 1 and x <> 3;", SelectApi::query, false,
         "#aaaa1 = :vaaa1"},
        {"select * from ddbsh_ne where pk = 1 and not sk <> 5;", SelectApi::get_item, false, ""},
        {"select * from ddbsh_ne where pk <> 1;", SelectApi::scan, false, ""},
        {"select * from ddbsh_ne where pk <> 1 and sk = 5;", SelectApi::scan, false, ""},
        {"select * from ddbsh_ne where pk = 1 and sk <> 5 and sk > 2;", SelectApi::scan, false, ""},
        {"select * from ddbsh_ne where pk = 1 and sk <> 5 and sk <> 7;", SelectApi::scan, false,
         ""},
        {"select * from ddbsh_ne where pk = 1 and sk <> 5 and begins_with(sk, \"1\");",
         SelectApi::scan, false, ""},
        {"select * from ddbsh_ne where pk = 1 and (sk <> 5 or x = 1);", SelectApi::scan, false, ""},
        {"select * from ddbsh_ne where not (pk = 1 and sk <> 5);", SelectApi::scan, false, ""},
        {"select * from ddbsh_ne where pk = 1 and sk <> \"x\";", SelectApi::scan, false, ""},
        {"select * from ddbsh_ne where pk = 1 and sk <> null;", SelectApi::scan, false, ""},
        {"select * from ddbsh_ne where pk = 1 and sk is not null;", SelectApi::scan, false, ""},
        {"select * from ddbsh_ne.ne_lsi where pk = 1 and l <> 5;", SelectApi::query, true,
         "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2"},
        {"select * from ddbsh_ne.ne_gsi where g = \"a\" and h <> 5;", SelectApi::query, true,
         "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2"},
        {"select * from ddbsh_ne.ne_gsi where g = \"a\" and sk <> 5;", SelectApi::query, false,
         "#aaaa1 = :vaaa1"},
        {"select * from ddbsh_ne.ne_cgsi where g = \"a\" and q = \"z\" and h <> 5;",
         SelectApi::query, true, "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2 AND #aaaa3 < :vaaa3"},
        {"select * from ddbsh_ne.ne_cgsi where g = \"a\" and q = \"z\" and h = 5 and r <> 5;",
         SelectApi::query, true,
         "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2 AND #aaaa3 = :vaaa3 AND #aaaa4 < :vaaa4"},
        {"select * from ddbsh_ne.ne_cgsi where g = \"a\" and q = \"z\" and h <> 5 and r = 5;",
         SelectApi::scan, false, ""},
        {"select * from ddbsh_ne.ne_cgsi where g = \"a\" and h <> 5;", SelectApi::scan, false, ""},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.sql);
        ddbsh::test::FakeDDBClient client;
        client.set_table(ne_table());
        ddbsh::Session session = make_session(client);
        SelectPlan plan = plan_ok(session, client, c.sql);
        ASSERT_EQ(plan.api, c.api);
        EXPECT_EQ(plan.query_upper.has_value(), c.split);
        if (c.api != SelectApi::query) {
            continue;
        }
        EXPECT_EQ(plan.query.key_condition_expression, c.lower_kc);
        if (c.split) {
            std::string upper = c.lower_kc;
            upper.replace(upper.find(" < "), 3, " > ");
            EXPECT_EQ(plan.query_upper->key_condition_expression, upper);
            expect_legs_match(plan.query, *plan.query_upper);
        }
    }
}

TEST(PlannerNotEqual, LegsShareFilterProjectionConsistencyAndCapacity) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(ne_table());
    ddbsh::Session session = make_session(client);
    SelectPlan plan =
        plan_ok(session, client,
                "select consistent x, sk from ddbsh_ne where pk = 1 and sk <> 5 and x <> 30 "
                "return consumed capacity total;");
    ASSERT_EQ(plan.api, SelectApi::query);
    ASSERT_TRUE(plan.query_upper.has_value());
    const auto& lo = plan.query;
    EXPECT_EQ(lo.projection_expression, std::optional<std::string>("#aaaa1, #aaaa2"));
    EXPECT_EQ(lo.key_condition_expression, "#aaaa3 = :vaaa1 AND #aaaa2 < :vaaa2");
    EXPECT_EQ(lo.filter_expression, std::optional<std::string>("#aaaa1 <> :vaaa3"));
    EXPECT_TRUE(lo.consistent_read);
    EXPECT_EQ(lo.return_consumed_capacity, ReturnConsumedCapacity::total);
    EXPECT_EQ(plan.query_upper->key_condition_expression, "#aaaa3 = :vaaa1 AND #aaaa2 > :vaaa2");
    expect_legs_match(lo, *plan.query_upper);
}

TEST(PlannerNotEqual, BangFilterKeepsItsSpellingOnBothLegs) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(ne_table());
    ddbsh::Session session = make_session(client);
    SelectPlan plan =
        plan_ok(session, client, "select * from ddbsh_ne where pk = 1 and sk != 5 and x != 30;");
    ASSERT_TRUE(plan.query_upper.has_value());
    EXPECT_EQ(plan.query.filter_expression, std::optional<std::string>("NOT (#aaaa3 = :vaaa3)"));
    expect_legs_match(plan.query, *plan.query_upper);
}

TEST(PlannerNotEqual, IndexLegsCarryIndexName) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(ne_table());
    ddbsh::Session session = make_session(client);
    SelectPlan plan =
        plan_ok(session, client, "select * from ddbsh_ne.ne_gsi where g = \"a\" and h <> 5;");
    ASSERT_TRUE(plan.query_upper.has_value());
    EXPECT_EQ(plan.query.index_name, std::optional<std::string>("ne_gsi"));
    EXPECT_EQ(plan.query_upper->index_name, std::optional<std::string>("ne_gsi"));
}

TEST(PlannerNotEqual, NoDeclaredTypeSplitsAnyScalar) {
    // FakeDDBClient fixtures without AttributeDefinitions: the type rule has
    // nothing to check, so a string literal on a sort key splits.
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a_b("t"));
    ddbsh::Session session = make_session(client);
    SelectPlan plan = plan_ok(session, client, "select * from t where a = 1 and b <> \"x\";");
    ASSERT_EQ(plan.api, SelectApi::query);
    EXPECT_TRUE(plan.query_upper.has_value());
}

}  // namespace
