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

}  // namespace
