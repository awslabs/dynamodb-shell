// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// write_exec_test.cpp — W5 part 2 INSERT/REPLACE + DROP TABLE execution tests.
//
// Exercises the non-EXPLAIN write/DDL paths offline via FakeDDBClient:
//   * INSERT builds a conditional PutItem (attribute_not_exists(#pk)) per row,
//     REPLACE builds an unconditional PutItem; one status token per row.
//   * The INSERT condition aliases the partition key through a fresh SymbolTable
//     per row, each seeded with the next per-statement alias prefix (arch §6);
//     REPLACE consumes no prefix.
//   * A failed PutItem (e.g. a conditional-check failure) propagates as an Error.
//   * DROP TABLE issues DeleteTable and (unless NOWAIT) waits for the table to
//     be gone; IF EXISTS on a missing table succeeds silently with no output.

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
using ddbsh::TableKeySchema;

DescribeTableOut table_pk(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{"id", ""};
    return t;
}

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

// ---- INSERT ----------------------------------------------------------------

TEST(WriteExec, InsertSingleRowAddsConditionAndRendersToken) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("insert into t ( id, name ) values ( 5, \"bob\" );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "INSERT\n");

    ASSERT_EQ(client.put_requests().size(), 1u);
    const auto& req = client.put_requests()[0];
    EXPECT_EQ(req.table_name, "t");
    // First statement of the session => alias prefix "aaa"; pk token #aaaa1.
    ASSERT_TRUE(req.condition_expression.has_value());
    EXPECT_EQ(*req.condition_expression, "attribute_not_exists(#aaaa1)");
    ASSERT_EQ(req.expression_attribute_names.size(), 1u);
    EXPECT_EQ(req.expression_attribute_names[0].first, "#aaaa1");
    EXPECT_EQ(req.expression_attribute_names[0].second, "id");
    // The item carries both columns.
    EXPECT_EQ(req.item.size(), 2u);
}

TEST(WriteExec, ReplaceIsUnconditionalAndConsumesNoPrefix) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("replace into t ( id, v ) values ( 1, \"a\" );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "REPLACE\n");

    ASSERT_EQ(client.put_requests().size(), 1u);
    const auto& req = client.put_requests()[0];
    EXPECT_FALSE(req.condition_expression.has_value());
    EXPECT_TRUE(req.expression_attribute_names.empty());
}

TEST(WriteExec, InsertMultiRowEmitsOneTokenPerRowWithFreshPrefixEach) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("insert into t ( id ) values ( 1 ), ( 2 ), ( 3 );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "INSERT\nINSERT\nINSERT\n");

    ASSERT_EQ(client.put_requests().size(), 3u);
    // Each row's condition SymbolTable consumes the next prefix: aaa, baa, caa.
    EXPECT_EQ(*client.put_requests()[0].condition_expression, "attribute_not_exists(#aaaa1)");
    EXPECT_EQ(*client.put_requests()[1].condition_expression, "attribute_not_exists(#abaa1)");
    EXPECT_EQ(*client.put_requests()[2].condition_expression, "attribute_not_exists(#acaa1)");
}

TEST(WriteExec, InsertConditionalFailurePropagatesError) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    client.set_put_error(Error{Errc::conditional_check_failed, "The conditional request failed",
                               std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("insert into t ( id ) values ( 1 );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::conditional_check_failed);
    EXPECT_EQ(out.error().message, "The conditional request failed");
}

TEST(WriteExec, InsertColumnValueCountMismatchIsError) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    // Two columns, one value: validate_rows rejects before any PutItem.
    auto out = session.run("insert into t ( id, name ) values ( 1 );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::invalid_argument);
    EXPECT_TRUE(client.put_requests().empty());
}

TEST(WriteExec, InsertMissingTablePropagatesError) {
    ddbsh::test::FakeDDBClient client;  // no table registered
    ddbsh::Session session = make_session(client);

    auto out = session.run("insert into nope ( id ) values ( 1 );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::no_such_table);
}

TEST(WriteExec, ExplainInsertReportsNotImplemented) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("explain insert into t ( id ) values ( 1 );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::not_implemented);
    EXPECT_TRUE(client.put_requests().empty());  // EXPLAIN contacts no service
}

// ---- DROP TABLE ------------------------------------------------------------

TEST(WriteExec, DropTableDeletesAndWaits) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("drop table t;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "DROP\n");
    ASSERT_EQ(client.deleted_tables().size(), 1u);
    EXPECT_EQ(client.deleted_tables()[0], "t");
    ASSERT_EQ(client.waited_gone().size(), 1u);
    EXPECT_EQ(client.waited_gone()[0], "t");
}

TEST(WriteExec, DropTableNowaitSkipsWaiter) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("drop table nowait t;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "DROP\n");
    EXPECT_EQ(client.deleted_tables().size(), 1u);
    EXPECT_TRUE(client.waited_gone().empty());
}

TEST(WriteExec, DropTableIfExistsMissingIsSilentNoDelete) {
    ddbsh::test::FakeDDBClient client;  // table "gone" not registered
    ddbsh::Session session = make_session(client);

    auto out = session.run("drop table if exists gone;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "");                   // no output
    EXPECT_TRUE(client.deleted_tables().empty());  // DeleteTable never issued
}

TEST(WriteExec, DropTableIfExistsPresentDeletes) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("drop table if exists t;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "DROP\n");
    EXPECT_EQ(client.deleted_tables().size(), 1u);
}

TEST(WriteExec, DropTableDeleteFailurePropagatesError) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    client.set_delete_error(
        Error{Errc::service_error, "Requested resource not found", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("drop table t;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_TRUE(client.waited_gone().empty());  // no wait after a failed delete
}

TEST(WriteExec, ExplainDropReportsNotImplemented) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("explain drop table t;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::not_implemented);
    EXPECT_TRUE(client.deleted_tables().empty());
}

}  // namespace
