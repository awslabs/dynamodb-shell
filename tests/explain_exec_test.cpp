// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// explain_exec_test.cpp — W6 EXPLAIN unit tests.
//
// Two layers, both offline (no AWS account):
//   1. Renderer: the explain_string (pretty, tab -> 3 spaces) and
//      strip_newlines (compact, one line, `:\t` -> `: `) transforms on a
//      hand-built ExplainOutcome, plus the trailing-status rule — the byte-exact
//      block shape the goldens expect (arch §9), validated independently of the
//      SDK.
//   2. End-to-end via Session + FakeDDBClient: that EXPLAIN of each in-scope
//      command plans the SAME access pattern as a real run and emits the right
//      ApiName block(s) in the right order, with the right per-block format and
//      trailing token. The FakeDDBClient returns a deterministic placeholder
//      body (NOT byte-exact SDK JSON — golden parity validates the real
//      serialization via AwsDDBClient, W7), so these tests pin the engine's
//      API-selection + block-assembly contract, not the JSON field order.

#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::DescribeTableOut;
using ddbsh::ExecOutcome;
using ddbsh::ExplainBlock;
using ddbsh::ExplainOutcome;
using ddbsh::TableKeySchema;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

DescribeTableOut table_pk(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{{"a"}, {}};
    return t;
}

DescribeTableOut table_pk_rk(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{{"a"}, {"b"}};
    return t;
}

// The default FakeDDBClient explain body, pretty- and compact-rendered.
constexpr const char* kPrettyBody = "{\n   \"stub\":   \"explain\"\n}";
constexpr const char* kCompactBody = "{ \"stub\": \"explain\" }";

// ---- Renderer-only transforms (hand-built ExplainOutcome) ------------------

TEST(ExplainRender, PrettyBlockReplacesTabsWithThreeSpaces) {
    ExplainOutcome e;
    e.blocks.push_back(
        ExplainBlock{"Scan", "{\n\t\"TableName\":\t\"t\"\n}", ExplainBlock::Format::pretty});
    EXPECT_EQ(render(ExecOutcome{e}), "Scan({\n   \"TableName\":   \"t\"\n})\n");
}

TEST(ExplainRender, CompactBlockCollapsesNewlinesAndColonTab) {
    ExplainOutcome e;
    e.blocks.push_back(ExplainBlock{"DescribeTable", "{\n\t\"TableName\":\t\"t\"\n}",
                                    ExplainBlock::Format::compact});
    EXPECT_EQ(render(ExecOutcome{e}), "DescribeTable({ \"TableName\": \"t\" })\n");
}

TEST(ExplainRender, MultipleBlocksEmittedInOrder) {
    ExplainOutcome e;
    e.blocks.push_back(ExplainBlock{"DescribeTable", "{}", ExplainBlock::Format::compact});
    e.blocks.push_back(ExplainBlock{"DescribeTimeToLive", "{}", ExplainBlock::Format::compact});
    EXPECT_EQ(render(ExecOutcome{e}), "DescribeTable({})\nDescribeTimeToLive({})\n");
}

TEST(ExplainRender, TrailingStatusFollowsBlocks) {
    ExplainOutcome e;
    e.blocks.push_back(ExplainBlock{"CreateTable", "{}", ExplainBlock::Format::pretty});
    e.trailing_status = "CREATE";
    EXPECT_EQ(render(ExecOutcome{e}), "CreateTable({})\nCREATE\n");
}

TEST(ExplainRender, TrailingStatusWithNoBlocks) {
    ExplainOutcome e;
    e.trailing_status = "CREATE";
    EXPECT_EQ(render(ExecOutcome{e}), "CREATE\n");
}

// ---- SELECT EXPLAIN: planner decision drives the ApiName -------------------

TEST(ExplainSelect, FullKeySelectsGetItem) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    auto session = make_session(client);
    auto outcome = session.run("explain select * from t where a = 3;");
    ASSERT_TRUE(outcome.has_value());
    const auto& e = std::get<ExplainOutcome>(*outcome);
    ASSERT_EQ(e.blocks.size(), 1u);
    EXPECT_EQ(e.blocks[0].api_name, "GetItem");
    EXPECT_FALSE(e.trailing_status.has_value());
    EXPECT_EQ(render(*outcome), std::string("GetItem(") + kPrettyBody + ")\n");
}

TEST(ExplainSelect, HashEqualityOnlySelectsQuery) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk_rk("t"));
    auto session = make_session(client);
    auto outcome = session.run("explain select * from t where a = 3;");
    ASSERT_TRUE(outcome.has_value());
    const auto& e = std::get<ExplainOutcome>(*outcome);
    ASSERT_EQ(e.blocks.size(), 1u);
    EXPECT_EQ(e.blocks[0].api_name, "Query");
}

TEST(ExplainSelect, NoWhereSelectsScan) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    auto session = make_session(client);
    auto outcome = session.run("explain select * from t;");
    ASSERT_TRUE(outcome.has_value());
    const auto& e = std::get<ExplainOutcome>(*outcome);
    ASSERT_EQ(e.blocks.size(), 1u);
    EXPECT_EQ(e.blocks[0].api_name, "Scan");
}

TEST(ExplainSelect, MissingTableIsError) {
    ddbsh::test::FakeDDBClient client;
    auto session = make_session(client);
    auto outcome = session.run("explain select * from nope;");
    EXPECT_FALSE(outcome.has_value());
}

// ---- Composite (multipart) GSI EXPLAIN SELECT (phase 4a, P4a-02 item 6) -----
// A deterministic, offline backstop for the seeded golden iexp-select-mpgsi:
// the index `firstgsi` has a composite HASH (a, b, c) and composite RANGE
// (d, e). A Query requires `=` on EVERY partition-key component; a partial
// composite hash demotes to Scan. Mirrors the golden's four shapes.
DescribeTableOut table_composite_gsi(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{{"a"}, {}};
    ddbsh::SecondaryIndexSchema g;
    g.index_name = "firstgsi";
    g.key_schema.hash_keys = {"a", "b", "c"};
    g.key_schema.range_keys = {"d", "e"};
    t.global_secondary_indexes.push_back(std::move(g));
    return t;
}

TEST(ExplainSelect, CompositeGsiPartialHashSelectsScan) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_composite_gsi("t"));
    auto session = make_session(client);

    for (const char* q : {"explain select * from t.firstgsi where a = 3;",
                          "explain select * from t.firstgsi where a = 3 and b = 4;"}) {
        auto outcome = session.run(q);
        ASSERT_TRUE(outcome.has_value()) << q;
        const auto& e = std::get<ExplainOutcome>(*outcome);
        ASSERT_EQ(e.blocks.size(), 1u) << q;
        EXPECT_EQ(e.blocks[0].api_name, "Scan") << q;
    }
}

TEST(ExplainSelect, CompositeGsiFullHashSelectsQuery) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_composite_gsi("t"));
    auto session = make_session(client);

    for (const char* q :
         {"explain select * from t.firstgsi where a = 3 and b = 4 and c = 5;",
          "explain select * from t.firstgsi where a = 3 and b = 4 and c = 5 and d = 6;"}) {
        auto outcome = session.run(q);
        ASSERT_TRUE(outcome.has_value()) << q;
        const auto& e = std::get<ExplainOutcome>(*outcome);
        ASSERT_EQ(e.blocks.size(), 1u) << q;
        EXPECT_EQ(e.blocks[0].api_name, "Query") << q;
    }
}

// ---- INSERT / REPLACE EXPLAIN: one PutItem block per row -------------------

TEST(ExplainInsert, OneBlockPerRowNoTrailingStatus) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    auto session = make_session(client);
    auto outcome = session.run("explain insert into t (a, b) values (1, 2), (3, 4), (5, 6);");
    ASSERT_TRUE(outcome.has_value());
    const auto& e = std::get<ExplainOutcome>(*outcome);
    ASSERT_EQ(e.blocks.size(), 3u);
    for (const auto& block : e.blocks) {
        EXPECT_EQ(block.api_name, "PutItem");
    }
    EXPECT_FALSE(e.trailing_status.has_value());
}

TEST(ExplainInsert, UnequalColumnsAndValuesIsError) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    auto session = make_session(client);
    auto outcome = session.run("explain insert into t (a, b) values (1);");
    EXPECT_FALSE(outcome.has_value());
}

// ---- CREATE TABLE EXPLAIN: CreateTable block + CREATE token ----------------

TEST(ExplainCreate, EmitsCreateTableBlockThenCreateToken) {
    ddbsh::test::FakeDDBClient client;
    auto session = make_session(client);
    auto outcome = session.run("explain create table t (a number) primary key (a hash);");
    ASSERT_TRUE(outcome.has_value());
    const auto& e = std::get<ExplainOutcome>(*outcome);
    ASSERT_EQ(e.blocks.size(), 1u);
    EXPECT_EQ(e.blocks[0].api_name, "CreateTable");
    ASSERT_TRUE(e.trailing_status.has_value());
    EXPECT_EQ(*e.trailing_status, "CREATE");
    EXPECT_EQ(render(*outcome), std::string("CreateTable(") + kPrettyBody + ")\nCREATE\n");
}

TEST(ExplainCreate, IfNotExistsOnExistingTableEmitsOnlyCreateToken) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    auto session = make_session(client);
    auto outcome =
        session.run("explain create table if not exists t (a number) primary key (a hash);");
    ASSERT_TRUE(outcome.has_value());
    const auto& e = std::get<ExplainOutcome>(*outcome);
    EXPECT_TRUE(e.blocks.empty());
    ASSERT_TRUE(e.trailing_status.has_value());
    EXPECT_EQ(*e.trailing_status, "CREATE");
    EXPECT_EQ(render(*outcome), "CREATE\n");
}

// ---- DROP TABLE EXPLAIN: DeleteTable block, no DROP token ------------------

TEST(ExplainDrop, EmitsDeleteTableBlockNoTrailingStatus) {
    ddbsh::test::FakeDDBClient client;
    auto session = make_session(client);
    auto outcome = session.run("explain drop table t;");
    ASSERT_TRUE(outcome.has_value());
    const auto& e = std::get<ExplainOutcome>(*outcome);
    ASSERT_EQ(e.blocks.size(), 1u);
    EXPECT_EQ(e.blocks[0].api_name, "DeleteTable");
    EXPECT_FALSE(e.trailing_status.has_value());
}

TEST(ExplainDrop, IfExistsMissingTableEmitsNothing) {
    ddbsh::test::FakeDDBClient client;
    auto session = make_session(client);
    auto outcome = session.run("explain drop table if exists nope;");
    ASSERT_TRUE(outcome.has_value());
    // A missing IF EXISTS target yields an EmptyOutcome (no DeleteTable block).
    EXPECT_FALSE(std::holds_alternative<ExplainOutcome>(*outcome));
    EXPECT_EQ(render(*outcome), "");
}

// ---- DESCRIBE EXPLAIN: four requests, mixed compact/pretty format ----------

TEST(ExplainDescribe, FourBlocksInOrderWithMixedFormat) {
    ddbsh::test::FakeDDBClient client;
    auto session = make_session(client);
    auto outcome = session.run("explain describe t;");
    ASSERT_TRUE(outcome.has_value());
    const auto& e = std::get<ExplainOutcome>(*outcome);
    ASSERT_EQ(e.blocks.size(), 4u);
    EXPECT_EQ(e.blocks[0].api_name, "DescribeTable");
    EXPECT_EQ(e.blocks[0].format, ExplainBlock::Format::compact);
    EXPECT_EQ(e.blocks[1].api_name, "DescribeTimeToLive");
    EXPECT_EQ(e.blocks[1].format, ExplainBlock::Format::compact);
    EXPECT_EQ(e.blocks[2].api_name, "DescribeContinuousBackups");
    EXPECT_EQ(e.blocks[2].format, ExplainBlock::Format::pretty);
    EXPECT_EQ(e.blocks[3].api_name, "DescribeTableReplicaAutoScaling");
    EXPECT_EQ(e.blocks[3].format, ExplainBlock::Format::pretty);

    const std::string expected = std::string("DescribeTable(") + kCompactBody + ")\n" +
                                 "DescribeTimeToLive(" + kCompactBody + ")\n" +
                                 "DescribeContinuousBackups(" + kPrettyBody + ")\n" +
                                 "DescribeTableReplicaAutoScaling(" + kPrettyBody + ")\n";
    EXPECT_EQ(render(*outcome), expected);
}

}  // namespace
