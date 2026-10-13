// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// create_table_exec_test.cpp — W5 part 2b CREATE TABLE execution tests.
//
// Exercises the non-EXPLAIN CREATE TABLE path offline via FakeDDBClient:
//   * CREATE issues CreateTable and (unless NOWAIT) waits for the table to
//     become ACTIVE; it renders the CREATE status token.
//   * The parsed table definition is faithfully copied into the CreateTableIn
//     request (name, attributes, key schema, billing, index set).
//   * A nullopt billing (no billing clause) is forwarded as nullopt so the
//     adapter applies the on-demand default (arch §8).
//   * IF NOT EXISTS probes the table first: on an existing table it skips the
//     CreateTable call but still renders CREATE; on a missing table it creates.
//   * A CreateTable failure (e.g. ResourceInUse "Table already exists")
//     propagates as an Error with the service message.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::BillingMode;
using ddbsh::DescribeTableOut;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::ScalarType;
using ddbsh::TableKeySchema;

DescribeTableOut table_pk(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{"a", ""};
    return t;
}

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

TEST(CreateTableExec, ProvisionedCreatesAndWaits) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( a number ) primary key ( a hash ) "
        "billing mode provisioned ( 200 rcu, 100 wcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");

    ASSERT_EQ(client.create_requests().size(), 1u);
    const auto& req = client.create_requests()[0];
    EXPECT_EQ(req.table_name, "t");
    ASSERT_EQ(req.attribute_definitions.size(), 1u);
    EXPECT_EQ(req.attribute_definitions[0].name, "a");
    EXPECT_EQ(req.attribute_definitions[0].type, ScalarType::n);
    EXPECT_EQ(req.key_schema.hash, "a");
    EXPECT_FALSE(req.key_schema.range.has_value());
    ASSERT_TRUE(req.billing.has_value());
    EXPECT_EQ(req.billing->mode, BillingMode::provisioned);
    EXPECT_EQ(req.billing->rcu, 200);
    EXPECT_EQ(req.billing->wcu, 100);

    ASSERT_EQ(client.waited_active().size(), 1u);
    EXPECT_EQ(client.waited_active()[0], "t");
}

TEST(CreateTableExec, NoBillingClauseForwardsNulloptForOnDemandDefault) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");

    ASSERT_EQ(client.create_requests().size(), 1u);
    // nullopt billing => the adapter applies the on-demand default (arch §8).
    EXPECT_FALSE(client.create_requests()[0].billing.has_value());
}

TEST(CreateTableExec, HashRangeKeySchemaCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out =
        session.run("create table t ( a number, b string ) primary key ( a hash, b range );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    const auto& req = client.create_requests()[0];
    EXPECT_EQ(req.key_schema.hash, "a");
    ASSERT_TRUE(req.key_schema.range.has_value());
    EXPECT_EQ(*req.key_schema.range, "b");
}

TEST(CreateTableExec, NowaitSkipsWaiter) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table nowait t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");
    EXPECT_EQ(client.create_requests().size(), 1u);
    EXPECT_TRUE(client.waited_active().empty());
}

TEST(CreateTableExec, IfNotExistsOnExistingSkipsCreateButStillRendersCreate) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));  // table already exists
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table if not exists t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");
    EXPECT_TRUE(client.create_requests().empty());  // no CreateTable issued
    EXPECT_TRUE(client.waited_active().empty());
}

TEST(CreateTableExec, IfNotExistsOnMissingCreates) {
    ddbsh::test::FakeDDBClient client;  // table not registered
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table if not exists t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");
    EXPECT_EQ(client.create_requests().size(), 1u);
}

TEST(CreateTableExec, CreateFailurePropagatesServiceMessage) {
    ddbsh::test::FakeDDBClient client;
    client.set_create_error(Error{Errc::service_error, "Table already exists: t",
                                  std::string{"ResourceInUseException"}, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table t ( a number ) primary key ( a hash );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "Table already exists: t");
    EXPECT_TRUE(client.waited_active().empty());  // no wait after a failed create
}

TEST(CreateTableExec, GsiAndLsiCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( a number, b string, c number ) primary key ( a hash, b range ) "
        "gsi ( myindex on ( c hash ) projecting all ) "
        "lsi ( myidx on ( a hash, c range ) projecting keys only );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    const auto& req = client.create_requests()[0];
    ASSERT_EQ(req.gsis.size(), 1u);
    EXPECT_EQ(req.gsis[0].name, "myindex");
    EXPECT_EQ(req.gsis[0].key_schema.hash, "c");
    ASSERT_EQ(req.lsis.size(), 1u);
    EXPECT_EQ(req.lsis[0].name, "myidx");
    EXPECT_EQ(req.lsis[0].key_schema.hash, "a");
    ASSERT_TRUE(req.lsis[0].key_schema.range.has_value());
    EXPECT_EQ(*req.lsis[0].key_schema.range, "c");
}

TEST(CreateTableExec, ExplainCreateReportsNotImplemented) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("explain create table t ( a number ) primary key ( a hash );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::not_implemented);
    EXPECT_TRUE(client.create_requests().empty());  // EXPLAIN contacts no service
}

}  // namespace
