// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// parse_test.cpp — W2 grammar + owning-AST unit tests (no backend).
//
// Exercises Session::parse() for the control/DDL/show command families ported
// in W2, asserting the parsed AST node contents, the EXPLAIN flag, string
// unquoting, and the byte-exact parse-error string. These are pure
// parse-level checks (execution lands in W4/W5), so they need no IDDBClient
// behavior; a trivial FakeDDBClient satisfies the Session's borrowed-client
// contract.

#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "ddbsh/session.hpp"
#include "ddbsh/statement.hpp"

#include "fake_ddb_client.hpp"
#include "parser/ast.hpp"

namespace {

using ddbsh::StatementKind;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

// Parse `text`, require success, and return the owned Statement.
ddbsh::Statement parse_ok(ddbsh::Session& session, const std::string& text) {
    auto result = session.parse(text);
    EXPECT_TRUE(result.has_value())
        << "parse failed: " << (result.has_value() ? std::string{} : result.error().message);
    return std::move(result.value());
}

template <class Node>
const Node& node_of(const ddbsh::Statement& stmt) {
    return std::get<Node>(stmt.ast()->command);
}

TEST(Parse, EmptyInput) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "");
    EXPECT_EQ(stmt.kind(), StatementKind::empty);
    EXPECT_FALSE(stmt.explain());
    EXPECT_EQ(stmt.ast(), nullptr);  // default-built empty statement owns no AST
}

TEST(Parse, BareSemicolonIsEmpty) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    EXPECT_EQ(parse_ok(session, ";").kind(), StatementKind::empty);
}

TEST(Parse, CommentOnlyIsEmpty) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    EXPECT_EQ(parse_ok(session, "-- just a comment\n").kind(), StatementKind::empty);
}

TEST(Parse, Quit) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "quit;");
    EXPECT_EQ(stmt.kind(), StatementKind::quit);
}

TEST(Parse, ShowVersion) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    EXPECT_EQ(parse_ok(session, "show version;").kind(), StatementKind::show_version);
}

TEST(Parse, ConnectRegionOnly) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "connect us-west-2;");
    ASSERT_EQ(stmt.kind(), StatementKind::connect);
    const auto& connect = node_of<ddbsh::engine::ConnectCmd>(stmt);
    EXPECT_EQ(connect.region, "us-west-2");  // hyphenated identifier is one token (arch §4)
    EXPECT_EQ(connect.endpoint, "");
}

TEST(Parse, ConnectWithEndpoint) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "connect us-east-1 with endpoint \"https://localhost:8000\";");
    ASSERT_EQ(stmt.kind(), StatementKind::connect);
    const auto& connect = node_of<ddbsh::engine::ConnectCmd>(stmt);
    EXPECT_EQ(connect.region, "us-east-1");
    EXPECT_EQ(connect.endpoint, "https://localhost:8000");  // outer quotes stripped
}

TEST(Parse, ShowTablesPlain) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "show tables;");
    ASSERT_EQ(stmt.kind(), StatementKind::show_tables);
    const auto& show = node_of<ddbsh::engine::ShowTablesCmd>(stmt);
    EXPECT_FALSE(show.brief);
    EXPECT_EQ(show.like, "");
}

TEST(Parse, ShowTablesBriefLike) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "show tables brief like \"^bal.*\";");
    ASSERT_EQ(stmt.kind(), StatementKind::show_tables);
    const auto& show = node_of<ddbsh::engine::ShowTablesCmd>(stmt);
    EXPECT_TRUE(show.brief);
    EXPECT_EQ(show.like, "^bal.*");
}

TEST(Parse, Describe) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "describe my_table;");
    ASSERT_EQ(stmt.kind(), StatementKind::describe);
    EXPECT_EQ(node_of<ddbsh::engine::DescribeCmd>(stmt).table, "my_table");
}

TEST(Parse, DropTablePlain) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "drop table my_table;");
    ASSERT_EQ(stmt.kind(), StatementKind::drop_table);
    const auto& drop = node_of<ddbsh::engine::DropTableCmd>(stmt);
    EXPECT_EQ(drop.table, "my_table");
    EXPECT_FALSE(drop.if_exists);
    EXPECT_FALSE(drop.nowait);
}

TEST(Parse, DropTableIfExistsNowait) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "drop table if exists nowait my_table;");
    ASSERT_EQ(stmt.kind(), StatementKind::drop_table);
    const auto& drop = node_of<ddbsh::engine::DropTableCmd>(stmt);
    EXPECT_EQ(drop.table, "my_table");
    EXPECT_TRUE(drop.if_exists);
    EXPECT_TRUE(drop.nowait);
}

TEST(Parse, ExplainDropTableSetsFlag) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "explain drop table my_table;");
    EXPECT_EQ(stmt.kind(), StatementKind::drop_table);
    EXPECT_TRUE(stmt.explain());
}

TEST(Parse, ExplainDescribeSetsFlag) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "explain describe my_table;");
    EXPECT_EQ(stmt.kind(), StatementKind::describe);
    EXPECT_TRUE(stmt.explain());
}

TEST(Parse, DoubleQuotedTableName) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    // A double-quoted identifier keeps embedded spaces; outer quotes stripped.
    auto stmt = parse_ok(session, "describe \"table with spaces\";");
    EXPECT_EQ(node_of<ddbsh::engine::DescribeCmd>(stmt).table, "table with spaces");
}

TEST(Parse, SyntaxErrorIsParseErrorWithLocation) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    // "show tablas;" — "tablas" is not a valid SHOW target (arch: the oracle's
    // ddbsh-basic-invalid-cli case). The failure is a Result error, never an
    // exception or exit (D12), and carries the composed "[line, col] syntax
    // error at <lexeme>" string (tenet 5).
    auto result = session.parse("show tablas;");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ddbsh::Errc::parse_error);
    EXPECT_NE(result.error().message.find("syntax error"), std::string::npos)
        << result.error().message;
}

TEST(Parse, ExplainIsNotAllowedOnShow) {
    // EXPLAIN wraps ddl/dml only (as the original did); EXPLAIN SHOW is invalid.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto result = session.parse("explain show tables;");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ddbsh::Errc::parse_error);
}

// ---- CREATE TABLE (W2 part 2) ----------------------------------------------

TEST(Parse, CreateTableMinimalHashOnly) {
    // No billing clause: the AST carries nullopt (on-demand default is applied
    // at execution, not parse — matching the original create_table.cpp).
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "create table t ( id number ) primary key ( id hash );");
    ASSERT_EQ(stmt.kind(), StatementKind::create_table);
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    EXPECT_EQ(ct.table_name, "t");
    EXPECT_FALSE(ct.if_not_exists);
    EXPECT_FALSE(ct.nowait);
    ASSERT_EQ(ct.attribute_definitions.size(), 1u);
    EXPECT_EQ(ct.attribute_definitions[0].name, "id");
    EXPECT_EQ(ct.attribute_definitions[0].type, ddbsh::engine::ScalarType::n);
    EXPECT_EQ(ct.key_schema.hash, "id");
    EXPECT_FALSE(ct.key_schema.range.has_value());
    EXPECT_FALSE(ct.billing.has_value());
    EXPECT_FALSE(ct.warm.has_value());
    EXPECT_TRUE(ct.gsis.empty());
    EXPECT_TRUE(ct.lsis.empty());
    EXPECT_FALSE(ct.stream.has_value());
    EXPECT_FALSE(ct.sse.has_value());
    EXPECT_EQ(ct.table_class, ddbsh::engine::TableClass::standard);
    EXPECT_FALSE(ct.deletion_protection);
    EXPECT_TRUE(ct.tags.empty());
}

TEST(Parse, CreateTableIfNotExistsNowaitHashRange) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "create table if not exists nowait t ( pk string, sk number ) "
                         "primary key ( pk hash, sk range );");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    EXPECT_TRUE(ct.if_not_exists);
    EXPECT_TRUE(ct.nowait);
    ASSERT_EQ(ct.attribute_definitions.size(), 2u);
    EXPECT_EQ(ct.attribute_definitions[0].type, ddbsh::engine::ScalarType::s);
    EXPECT_EQ(ct.attribute_definitions[1].type, ddbsh::engine::ScalarType::n);
    EXPECT_EQ(ct.key_schema.hash, "pk");
    ASSERT_TRUE(ct.key_schema.range.has_value());
    EXPECT_EQ(*ct.key_schema.range, "sk");
}

TEST(Parse, CreateTableRangeHashReordersToHashFirst) {
    // Written RANGE, HASH; the AST normalizes to [HASH, RANGE] as the original.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt =
        parse_ok(session, "create table t ( a string, b string ) primary key ( a range, b hash );");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    EXPECT_EQ(ct.key_schema.hash, "b");
    ASSERT_TRUE(ct.key_schema.range.has_value());
    EXPECT_EQ(*ct.key_schema.range, "a");
}

TEST(Parse, CreateTableProvisionedThroughput) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "create table t ( id number ) primary key ( id hash ) "
                         "billing mode provisioned ( 5 rcu, 7 wcu );");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    ASSERT_TRUE(ct.billing.has_value());
    EXPECT_EQ(ct.billing->mode, ddbsh::engine::BillingMode::provisioned);
    EXPECT_EQ(ct.billing->rcu, 5);
    EXPECT_EQ(ct.billing->wcu, 7);
}

TEST(Parse, CreateTableProvisionedThroughputWcuFirst) {
    // The RCU/WCU clause is order-independent (arch §8): WCU first still maps
    // correctly.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "create table t ( id number ) primary key ( id hash ) "
                         "billing mode provisioned ( 7 wcu, 5 rcu );");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    ASSERT_TRUE(ct.billing.has_value());
    EXPECT_EQ(ct.billing->rcu, 5);
    EXPECT_EQ(ct.billing->wcu, 7);
}

TEST(Parse, CreateTableOnDemandBareHasNoThroughputUnits) {
    // `BILLING MODE ON DEMAND` with no THROUGHPUT clause: present billing, but
    // on-demand max units unset (distinct from the no-billing default — the
    // original emits an empty OnDemandThroughput here, nothing there).
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "create table t ( id number ) primary key ( id hash ) "
                         "billing mode on demand;");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    ASSERT_TRUE(ct.billing.has_value());
    EXPECT_EQ(ct.billing->mode, ddbsh::engine::BillingMode::pay_per_request);
    EXPECT_FALSE(ct.billing->od.max_read_request_units.has_value());
    EXPECT_FALSE(ct.billing->od.max_write_request_units.has_value());
}

TEST(Parse, CreateTableOnDemandThroughputUnlimited) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "create table t ( id number ) primary key ( id hash ) "
                         "billing mode on demand throughput unlimited;");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    ASSERT_TRUE(ct.billing.has_value());
    ASSERT_TRUE(ct.billing->od.max_read_request_units.has_value());
    EXPECT_EQ(*ct.billing->od.max_read_request_units, -1);
    EXPECT_EQ(*ct.billing->od.max_write_request_units, -1);
}

TEST(Parse, CreateTableWarmThroughput) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "create table t ( id number ) primary key ( id hash ) "
                         "warm throughput ( 100 rcu, 200 wcu );");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    ASSERT_TRUE(ct.warm.has_value());
    EXPECT_EQ(ct.warm->read_units_per_second, 100);
    EXPECT_EQ(ct.warm->write_units_per_second, 200);
}

TEST(Parse, CreateTableGsiAndLsi) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(
        session,
        "create table t ( pk string, sk string, g string ) primary key ( pk hash, sk range ) "
        "gsi ( gsi1 on ( g hash ) projecting all ) "
        "lsi ( lsi1 on ( pk hash, g range ) projecting include ( sk ) );");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    ASSERT_EQ(ct.gsis.size(), 1u);
    EXPECT_EQ(ct.gsis[0].name, "gsi1");
    EXPECT_EQ(ct.gsis[0].key_schema.hash, "g");
    EXPECT_EQ(ct.gsis[0].projection.type, ddbsh::engine::ProjectionType::all);
    ASSERT_EQ(ct.lsis.size(), 1u);
    EXPECT_EQ(ct.lsis[0].name, "lsi1");
    EXPECT_EQ(ct.lsis[0].projection.type, ddbsh::engine::ProjectionType::include);
    ASSERT_EQ(ct.lsis[0].projection.non_key_attributes.size(), 1u);
    EXPECT_EQ(ct.lsis[0].projection.non_key_attributes[0], "sk");
}

TEST(Parse, CreateTableStreamSseClassDeletionTags) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "create table t ( id number ) primary key ( id hash ) "
                         "stream ( both images ) "
                         "sse ( keyid \"abc\" kms ) "
                         "table class standard infrequent access "
                         "deletion protection enabled "
                         "tags ( env : prod, team : db );");
    const auto& ct = node_of<ddbsh::engine::CreateTableCmd>(stmt);
    ASSERT_TRUE(ct.stream.has_value());
    EXPECT_TRUE(ct.stream->enabled);
    EXPECT_EQ(ct.stream->view_type, ddbsh::engine::StreamViewType::new_and_old_images);
    ASSERT_TRUE(ct.sse.has_value());
    EXPECT_TRUE(ct.sse->enabled);
    ASSERT_TRUE(ct.sse->kms_master_key_id.has_value());
    EXPECT_EQ(*ct.sse->kms_master_key_id, "abc");
    EXPECT_EQ(ct.sse->sse_type, ddbsh::engine::SseType::kms);
    EXPECT_EQ(ct.table_class, ddbsh::engine::TableClass::standard_infrequent_access);
    EXPECT_TRUE(ct.deletion_protection);
    ASSERT_EQ(ct.tags.size(), 2u);
    EXPECT_EQ(ct.tags[0].key, "env");
    EXPECT_EQ(ct.tags[0].value, "prod");
    EXPECT_EQ(ct.tags[1].key, "team");
    EXPECT_EQ(ct.tags[1].value, "db");
}

TEST(Parse, ExplainCreateTableSetsFlag) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "explain create table t ( id number ) primary key ( id hash );");
    EXPECT_EQ(stmt.kind(), StatementKind::create_table);
    EXPECT_TRUE(stmt.explain());
}

}  // namespace
