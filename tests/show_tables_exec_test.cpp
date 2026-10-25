// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// show_tables_exec_test.cpp — W5 part 2c-i SHOW TABLES execution tests.
//
// Exercises the non-EXPLAIN SHOW TABLES path offline via FakeDDBClient:
//   * ListTables names are described per-table (DescribeTable + DescribeTTL) and
//     shaped into the pipe-delimited status row (arch §7), byte-for-byte with
//     the original show_tables.cpp printf.
//   * BRIEF prints just the names; LIKE filters by regex.
//   * A per-table describe/ttl failure collapses that row to just the name
//     (the original's has_dtr() && has_ttl() guard).
//   * The PAY_PER_REQUEST* / STANDARD* inferred-default markers and the TTL
//     "(attr)" suffix reproduce the original's defaulting.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::DescribeTableOut;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::SecondaryIndexSchema;
using ddbsh::TableKeySchema;
using ddbsh::TimeToLiveOut;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

// A provisioned table with a BillingModeSummary, a table-class summary, two
// GSIs and one LSI, and known identifiers — a fully-populated describe.
DescribeTableOut provisioned_table(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{{"pk"}, {"rk"}};
    t.table_status = "ACTIVE";
    t.table_id = "id-123";
    t.table_arn = "arn:aws:dynamodb:us-east-1:1:table/" + name;
    t.billing_mode_summary = "PROVISIONED";
    t.provisioned_throughput_set = true;
    t.table_class_summary = "STANDARD";
    t.global_secondary_indexes = {
        SecondaryIndexSchema{.index_name = "g1", .key_schema = TableKeySchema{{"a"}, {}}},
        SecondaryIndexSchema{.index_name = "g2", .key_schema = TableKeySchema{{"b"}, {}}}};
    t.local_secondary_indexes = {
        SecondaryIndexSchema{.index_name = "l1", .key_schema = TableKeySchema{{"pk"}, {"c"}}}};
    return t;
}

TEST(ShowTables, FullStatusRow) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"t1"});
    client.set_table(provisioned_table("t1"));
    client.set_ttl("t1", TimeToLiveOut{"DISABLED", ""});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out),
              "t1 | ACTIVE | PROVISIONED | STANDARD | id-123 | "
              "arn:aws:dynamodb:us-east-1:1:table/t1 | TTL DISABLED | GSI: 2 | LSI : 1 |\n");
}

TEST(ShowTables, InferredDefaultMarkersForOnDemandAndNoTableClass) {
    // No BillingModeSummary and no ProvisionedThroughput => PAY_PER_REQUEST*.
    // No TableClassSummary => STANDARD*. (The "*" marks the inferred default.)
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"od"});
    DescribeTableOut t;
    t.table_name = "od";
    t.key_schema = TableKeySchema{{"pk"}, {}};
    t.table_status = "ACTIVE";
    t.table_id = "idod";
    t.table_arn = "arnod";
    // billing_mode_summary unset, provisioned_throughput_set=false,
    // table_class_summary unset.
    client.set_table(t);
    client.set_ttl("od", TimeToLiveOut{"DISABLED", ""});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out),
              "od | ACTIVE | PAY_PER_REQUEST* | STANDARD* | idod | arnod | "
              "TTL DISABLED | GSI: 0 | LSI : 0 |\n");
}

TEST(ShowTables, ProvisionedFallbackWhenNoBillingSummary) {
    // No BillingModeSummary but ProvisionedThroughput present => "PROVISIONED"
    // (no "*", because the value is observed, not inferred).
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"p"});
    DescribeTableOut t;
    t.table_name = "p";
    t.key_schema = TableKeySchema{{"pk"}, {}};
    t.table_status = "ACTIVE";
    t.table_id = "idp";
    t.table_arn = "arnp";
    t.provisioned_throughput_set = true;
    t.table_class_summary = "STANDARD_INFREQUENT_ACCESS";
    client.set_table(t);
    client.set_ttl("p", TimeToLiveOut{"DISABLED", ""});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out),
              "p | ACTIVE | PROVISIONED | STANDARD_INFREQUENT_ACCESS | idp | arnp | "
              "TTL DISABLED | GSI: 0 | LSI : 0 |\n");
}

TEST(ShowTables, TtlEnabledShowsAttributeName) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"t1"});
    client.set_table(provisioned_table("t1"));
    client.set_ttl("t1", TimeToLiveOut{"ENABLED", "expires_at"});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(render(*out).find("TTL ENABLED (expires_at)"), std::string::npos);
}

TEST(ShowTables, TtlEnablingShowsAttributeName) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"t1"});
    client.set_table(provisioned_table("t1"));
    client.set_ttl("t1", TimeToLiveOut{"ENABLING", "ttlcol"});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(render(*out).find("TTL ENABLING (ttlcol)"), std::string::npos);
}

TEST(ShowTables, BriefPrintsOnlyNames) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"alpha", "beta", "gamma"});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables brief;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "alpha\nbeta\ngamma\n");
}

TEST(ShowTables, LikeFiltersByRegex) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"users", "orders", "user_sessions"});
    ddbsh::Session session = make_session(client);

    // regex_match is a FULL match (as the original std::regex_match), so the
    // pattern must span the whole name.
    auto out = session.run("show tables brief like \"user.*\";");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "users\nuser_sessions\n");
}

TEST(ShowTables, DescribeFailureCollapsesRowToName) {
    // A table is listed but cannot be described (e.g. deleted between the list
    // and the describe) => the row collapses to just the name.
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"ghost"});
    // No set_table("ghost") => describe_table returns no_such_table.
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "ghost\n");
}

TEST(ShowTables, TtlFailureCollapsesRowToName) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"t1"});
    client.set_table(provisioned_table("t1"));
    client.set_ttl_error("t1", Error{Errc::service_error, "ttl boom", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "t1\n");
}

TEST(ShowTables, EmptyAccountRendersNothing) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "");
}

TEST(ShowTables, ListTablesErrorPropagates) {
    ddbsh::test::FakeDDBClient client;
    client.set_list_tables_error(
        Error{Errc::service_error, "list boom", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show tables;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "list boom");
}

TEST(ShowTables, InvalidLikePatternIsErrorNotCrash) {
    ddbsh::test::FakeDDBClient client;
    client.set_table_names({"t1"});
    ddbsh::Session session = make_session(client);

    // An unbalanced bracket is an invalid ECMAScript regex; the engine reports
    // an Error rather than throwing across the boundary (D12).
    auto out = session.run("show tables like \"[unterminated\";");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::parse_error);
}

}  // namespace
