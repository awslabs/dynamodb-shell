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

#include "expressions/symbol_table.hpp"
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

// ---- INSERT / REPLACE (W2 part 2) ------------------------------------------

TEST(Parse, InsertSingleRow) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "insert into t ( id, name ) values ( 5, \"bob\" );");
    ASSERT_EQ(stmt.kind(), StatementKind::insert);
    const auto& ins = node_of<ddbsh::engine::InsertCmd>(stmt);
    EXPECT_EQ(ins.table_name, "t");
    EXPECT_TRUE(ins.insert);
    EXPECT_FALSE(ins.ratelimit.has_value());
    EXPECT_EQ(ins.return_value, ddbsh::engine::ReturnValue::none);
    ASSERT_EQ(ins.columns.size(), 2u);
    EXPECT_EQ(ins.columns[0], "id");
    EXPECT_EQ(ins.columns[1], "name");
    ASSERT_EQ(ins.rows.size(), 1u);
    ASSERT_EQ(ins.rows[0].size(), 2u);
    EXPECT_EQ(ins.rows[0][0].type, ddbsh::Value::Type::N);
    EXPECT_EQ(ins.rows[0][0].s, "5");  // exact lexeme preserved
    EXPECT_EQ(ins.rows[0][1].type, ddbsh::Value::Type::S);
    EXPECT_EQ(ins.rows[0][1].s, "bob");  // outer quotes stripped
}

TEST(Parse, InsertMultiRow) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "insert into t ( id ) values ( 1 ), ( 2 ), ( 3 );");
    const auto& ins = node_of<ddbsh::engine::InsertCmd>(stmt);
    ASSERT_EQ(ins.rows.size(), 3u);
    EXPECT_EQ(ins.rows[0][0].s, "1");
    EXPECT_EQ(ins.rows[1][0].s, "2");
    EXPECT_EQ(ins.rows[2][0].s, "3");
}

TEST(Parse, InsertScalarValueKinds) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(
        session, "insert into t ( a, b, c, d, e ) values ( 1.5, \"s\", true, false, null );");
    const auto& ins = node_of<ddbsh::engine::InsertCmd>(stmt);
    ASSERT_EQ(ins.rows[0].size(), 5u);
    EXPECT_EQ(ins.rows[0][0].type, ddbsh::Value::Type::N);
    EXPECT_EQ(ins.rows[0][0].s, "1.5");
    EXPECT_EQ(ins.rows[0][1].type, ddbsh::Value::Type::S);
    EXPECT_EQ(ins.rows[0][2].type, ddbsh::Value::Type::BOOL);
    EXPECT_TRUE(ins.rows[0][2].boolean);
    EXPECT_EQ(ins.rows[0][3].type, ddbsh::Value::Type::BOOL);
    EXPECT_FALSE(ins.rows[0][3].boolean);
    EXPECT_EQ(ins.rows[0][4].type, ddbsh::Value::Type::NUL);
}

TEST(Parse, InsertNestedListAndMap) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt =
        parse_ok(session, "insert into t ( l, m ) values ( [ 1, \"x\", [] ], { a : 1, b : {} } );");
    const auto& ins = node_of<ddbsh::engine::InsertCmd>(stmt);
    const auto& list = ins.rows[0][0];
    ASSERT_EQ(list.type, ddbsh::Value::Type::L);
    ASSERT_EQ(list.list.size(), 3u);
    EXPECT_EQ(list.list[0].type, ddbsh::Value::Type::N);
    EXPECT_EQ(list.list[1].type, ddbsh::Value::Type::S);
    EXPECT_EQ(list.list[2].type, ddbsh::Value::Type::L);  // nested empty list
    EXPECT_TRUE(list.list[2].list.empty());
    const auto& map = ins.rows[0][1];
    ASSERT_EQ(map.type, ddbsh::Value::Type::M);
    ASSERT_EQ(map.map.size(), 2u);
    EXPECT_EQ(map.map[0].first, "a");  // source order preserved
    EXPECT_EQ(map.map[0].second.type, ddbsh::Value::Type::N);
    EXPECT_EQ(map.map[1].first, "b");
    EXPECT_EQ(map.map[1].second.type, ddbsh::Value::Type::M);  // nested empty map
    EXPECT_TRUE(map.map[1].second.map.empty());
}

TEST(Parse, InsertEmptyTopLevelCollectionIsParseError) {
    // Faithful to the original: insert_value has no top-level empty []/{} rule
    // (only lee_rhs does), so VALUES ( [] ) is a syntax error.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto result = session.parse("insert into t ( a ) values ( [] );");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ddbsh::Errc::parse_error);
}

TEST(Parse, InsertWithRatelimit) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt =
        parse_ok(session, "insert into t ( id ) values ( 1 ) with ratelimit ( 10 rcu, 20 wcu );");
    const auto& ins = node_of<ddbsh::engine::InsertCmd>(stmt);
    ASSERT_TRUE(ins.ratelimit.has_value());
    EXPECT_DOUBLE_EQ(ins.ratelimit->rcu, 10.0);
    EXPECT_DOUBLE_EQ(ins.ratelimit->wcu, 20.0);
}

TEST(Parse, ReplaceIsUnconditionalWithReturnValue) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt =
        parse_ok(session, "replace into t ( id, v ) values ( 1, \"a\" ) return values all old;");
    ASSERT_EQ(stmt.kind(), StatementKind::insert);
    const auto& ins = node_of<ddbsh::engine::InsertCmd>(stmt);
    EXPECT_FALSE(ins.insert);  // REPLACE is unconditional
    EXPECT_EQ(ins.return_value, ddbsh::engine::ReturnValue::all_old);
}

TEST(Parse, ExplainInsertSetsFlag) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "explain insert into t ( id ) values ( 1 );");
    EXPECT_EQ(stmt.kind(), StatementKind::insert);
    EXPECT_TRUE(stmt.explain());
}

// ---- SELECT (W3 part 2) ----------------------------------------------------

// Serialize a parsed WHERE tree as a scan filter with a fixed-prefix symbol
// table, so a test can assert the whole expression tree parsed into the right
// structure (arch §6 alias scheme). Precondition: the SelectCmd has a WHERE.
std::string scan_filter_of(const ddbsh::engine::SelectCmd& select) {
    ddbsh::engine::SymbolTable st("aaa");
    return select.where->scan_filter_expression(&st);
}

TEST(Parse, SelectStarNoWhere) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "select * from t;");
    ASSERT_EQ(stmt.kind(), StatementKind::select_statement);
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    EXPECT_FALSE(sel.consistent);
    EXPECT_TRUE(sel.projection.empty());  // `*` => project all
    EXPECT_EQ(sel.table, "t");
    EXPECT_FALSE(sel.index.has_value());
    EXPECT_EQ(sel.where, nullptr);  // no WHERE clause
    EXPECT_EQ(sel.rcc, ddbsh::engine::ReturnConsumedCapacity::none);
    EXPECT_FALSE(sel.ratelimit.has_value());
}

TEST(Parse, SelectProjectionPaths) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    // Dotted and subscripted attribute paths are preserved verbatim in the
    // projection list (the symbol table tokenizes them later).
    auto stmt = parse_ok(session, "select a, b.c, d[0] from t;");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    ASSERT_EQ(sel.projection.size(), 3u);
    EXPECT_EQ(sel.projection[0], "a");
    EXPECT_EQ(sel.projection[1], "b.c");
    EXPECT_EQ(sel.projection[2], "d[0]");
}

TEST(Parse, SelectConsistent) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "select consistent * from t;");
    EXPECT_TRUE(node_of<ddbsh::engine::SelectCmd>(stmt).consistent);
}

TEST(Parse, SelectTableDotIndex) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "select * from t.idx;");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    EXPECT_EQ(sel.table, "t");
    ASSERT_TRUE(sel.index.has_value());
    EXPECT_EQ(*sel.index, "idx");
}

TEST(Parse, SelectWhereEquality) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "select * from t where id = 5;");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    ASSERT_NE(sel.where, nullptr);
    // Full key equality on a hash-only table is a GetItem (arch §8).
    EXPECT_TRUE(sel.where->can_getitem("id", ""));
    EXPECT_EQ(scan_filter_of(sel), "#aaaa1 = :vaaa1");
}

TEST(Parse, SelectWhereNotEqualDesugars) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "select * from t where id != 5;");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    EXPECT_EQ(scan_filter_of(sel), "NOT (#aaaa1 = :vaaa1)");  // arch §14 item 4
}

TEST(Parse, SelectWhereAndOr) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "select * from t where a = 1 and b = 2;");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    EXPECT_EQ(scan_filter_of(sel), "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2");
}

TEST(Parse, SelectWhereIsNotNullDesugars) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    // IS NOT NULL => NOT (a = <null>) AND attribute_exists(a)  (arch §6).
    auto stmt = parse_ok(session, "select * from t where a is not null;");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    EXPECT_EQ(scan_filter_of(sel), "NOT (#aaaa1 = :vaaa1) AND attribute_exists(#aaaa1)");
}

TEST(Parse, SelectWhereBetweenInBeginsWithContains) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "select * from t where a between 1 and 9 and b in ( 1, 2 ) and "
                         "begins_with(c, \"pre\") and contains(d, \"x\");");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    EXPECT_EQ(scan_filter_of(sel),
              "#aaaa1 BETWEEN :vaaa1 AND :vaaa2 AND #aaaa2 IN (:vaaa3, :vaaa4) AND "
              "begins_with(#aaaa3, :vaaa5) AND contains(#aaaa4, :vaaa6)");
}

TEST(Parse, SelectWhereAttributeExistsTypeAndSize) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "select * from t where attribute_exists(a) and "
                         "attribute_not_exists(b) and attribute_type(c, number) and size(d) > 3;");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    // attribute_not_exists renders as NOT (attribute_exists(...)). size(d)
    // becomes the literal lhs "size(d)" which the symbol table aliases AS A
    // WHOLE (one attribute name) — faithfully reproducing the original's
    // behavior (CLogicalComparison stores "size(" + attr + ")" verbatim and
    // passes it through new_attribute, which only splits on '.'/'['). This is
    // a latent oracle quirk (the DynamoDB-correct form would alias only the
    // inner attribute); no golden exercises size(), and we preserve the
    // original behavior rather than diverge silently (tenet 2).
    EXPECT_EQ(scan_filter_of(sel),
              "attribute_exists(#aaaa1) AND NOT (attribute_exists(#aaaa2)) AND "
              "attribute_type(#aaaa3, :vaaa1) AND #aaaa4 > :vaaa2");
}

TEST(Parse, SelectReturnConsumedCapacity) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    EXPECT_EQ(node_of<ddbsh::engine::SelectCmd>(
                  parse_ok(session, "select * from t return consumed capacity total;"))
                  .rcc,
              ddbsh::engine::ReturnConsumedCapacity::total);
    EXPECT_EQ(node_of<ddbsh::engine::SelectCmd>(
                  parse_ok(session, "select * from t return consumed capacity indexes;"))
                  .rcc,
              ddbsh::engine::ReturnConsumedCapacity::indexes);
}

TEST(Parse, SelectRatelimitAndRccIsSemanticError) {
    // The original rejected combining a ratelimit with RETURN CONSUMED CAPACITY
    // with a verbatim message (arch §9); reproduced byte-for-byte.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto result =
        session.parse("select * from t return consumed capacity total with ratelimit ( 10 rcu );");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ddbsh::Errc::invalid_argument);
    EXPECT_EQ(result.error().message,
              "You cannot specify both a ratelimit and a return_consumed_capacity.");
}

TEST(Parse, SelectWithRatelimitAlone) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "select * from t with ratelimit ( 10 rcu, 20 wcu );");
    const auto& sel = node_of<ddbsh::engine::SelectCmd>(stmt);
    ASSERT_TRUE(sel.ratelimit.has_value());
    EXPECT_DOUBLE_EQ(sel.ratelimit->rcu, 10.0);
    EXPECT_DOUBLE_EQ(sel.ratelimit->wcu, 20.0);
}

TEST(Parse, ExplainSelectSetsFlag) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "explain select * from t where id = 5;");
    EXPECT_EQ(stmt.kind(), StatementKind::select_statement);
    EXPECT_TRUE(stmt.explain());
}

// ---- ALTER TABLE combined `SET billing ... UPDATE GSI` gate (D-2e-6) --------
// The one piece of logic unique to 2e-2 lives in the grammar action: a combined
// `SET billing ... UPDATE GSI (...)` whose table billing mode is PROVISIONED
// keeps the GSI updates; on-demand drops them (the index throughput is implied)
// and records the informational diagnostic. The only prior coverage set the
// flag on a hand-built node, bypassing the grammar — these tests drive the
// decision through the parser itself (Should-fix, 2e-2 review).

TEST(Parse, AlterTableCombinedProvisionedKeepsGsiUpdates) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session,
                         "alter table t set billing mode provisioned ( 100 rcu, 200 wcu ) "
                         "update gsi ( g1 set billing mode provisioned ( 10 rcu, 20 wcu ) );");
    EXPECT_EQ(stmt.kind(), StatementKind::alter_table);
    const auto& alter = node_of<ddbsh::engine::AlterTableCmd>(stmt);
    ASSERT_TRUE(alter.billing.has_value());
    EXPECT_EQ(alter.billing->mode, ddbsh::engine::BillingMode::provisioned);
    EXPECT_FALSE(alter.ignored_gsi_on_demand);
    ASSERT_EQ(alter.gsi_updates.size(), 1u);
    EXPECT_EQ(alter.gsi_updates[0].action, ddbsh::engine::GsiUpdate::Action::update);
    EXPECT_EQ(alter.gsi_updates[0].index_name, "g1");
}

TEST(Parse, AlterTableCombinedOnDemandDropsGsiUpdates) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt =
        parse_ok(session,
                 "alter table t set billing mode on demand "
                 "update gsi ( g1 set billing mode on demand throughput (10 rcu, 20 wcu) );");
    EXPECT_EQ(stmt.kind(), StatementKind::alter_table);
    const auto& alter = node_of<ddbsh::engine::AlterTableCmd>(stmt);
    ASSERT_TRUE(alter.billing.has_value());
    EXPECT_EQ(alter.billing->mode, ddbsh::engine::BillingMode::pay_per_request);
    EXPECT_TRUE(alter.ignored_gsi_on_demand);  // gate fired
    EXPECT_TRUE(alter.gsi_updates.empty());    // dropped by the gate
}

// ---- ALTER TABLE SET TTL (2e-3) builds an AlterTtlCmd -----------------------

TEST(Parse, AlterTableSetTtlEnableBuildsTtlNode) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "alter table t set ttl ( expiry );");
    EXPECT_EQ(stmt.kind(), StatementKind::alter_table);
    const auto& ttl = node_of<ddbsh::engine::AlterTtlCmd>(stmt);
    EXPECT_EQ(ttl.table, "t");
    EXPECT_TRUE(ttl.enabled);
    EXPECT_EQ(ttl.attribute, "expiry");
}

TEST(Parse, AlterTableSetTtlDisableBuildsTtlNode) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = parse_ok(session, "alter table t set ttl disabled;");
    EXPECT_EQ(stmt.kind(), StatementKind::alter_table);
    const auto& ttl = node_of<ddbsh::engine::AlterTtlCmd>(stmt);
    EXPECT_EQ(ttl.table, "t");
    EXPECT_FALSE(ttl.enabled);
    EXPECT_TRUE(ttl.attribute.empty());
}

}  // namespace
