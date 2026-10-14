// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// batch_test.cpp — W7 multi-statement batch parsing (no backend).
//
// Exercises Session::parse_batch(), the engine's one-shot full-string entry
// point for the human-ratified isql-batch model (2026-10-14): the engine is
// handed a complete string, parses every `;`-separated statement in order,
// tolerates a missing trailing `;` on the final statement (the original string
// source appended one — arch §4), and surfaces a trailing parse error while
// still returning the statements that parsed before it. These are pure
// parse-level checks (no IDDBClient behavior needed); a trivial FakeDDBClient
// satisfies the Session's borrowed-client contract.
//
// The TokenTrace cases pin the architect's lookahead concern (2026-10-14): a
// fresh bison Parser per statement over a persistent scanner must not drop the
// next statement's leading token at the `;` boundary. If it did, the second
// statement would be missing or malformed.

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

TEST(Batch, EmptyStringYieldsNoStatements) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch("");
    EXPECT_TRUE(batch.statements.empty());
    EXPECT_FALSE(batch.error.has_value());
}

TEST(Batch, SingleStatementWithSemicolon) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch("show version;");
    ASSERT_EQ(batch.statements.size(), 1u);
    EXPECT_EQ(batch.statements[0].kind(), StatementKind::show_version);
    EXPECT_FALSE(batch.error.has_value());
}

TEST(Batch, SingleStatementNoSemicolon) {
    // The original string input source appended ';' when the -c payload lacked
    // one; many in-scope golden commands omit it (e.g. `drop table if exists
    // icd1`). The grammar accepts end-of-input in place of the final ';'.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch("drop table if exists icd1");
    ASSERT_EQ(batch.statements.size(), 1u);
    EXPECT_EQ(batch.statements[0].kind(), StatementKind::drop_table);
    EXPECT_FALSE(batch.error.has_value());
}

TEST(Batch, TwoStatements) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch("drop table if exists iins1; drop table if exists iins2;");
    ASSERT_EQ(batch.statements.size(), 2u);
    EXPECT_EQ(batch.statements[0].kind(), StatementKind::drop_table);
    EXPECT_EQ(batch.statements[1].kind(), StatementKind::drop_table);
    EXPECT_FALSE(batch.error.has_value());
}

TEST(Batch, ThreeStatementsFinalWithoutSemicolon) {
    // The iins-insert-check shape: three selects, no trailing ';'.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch =
        session.parse_batch("select * from iins1; select * from iins2; select * from iins3");
    ASSERT_EQ(batch.statements.size(), 3u);
    for (const auto& s : batch.statements) {
        EXPECT_EQ(s.kind(), StatementKind::select_statement);
    }
    EXPECT_FALSE(batch.error.has_value());
}

TEST(Batch, FiveInsertsWithTrailingSemicolon) {
    // The iins-insert-1 shape: five ';'-terminated INSERTs in one -c payload.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch(
        "insert into iins1 (a, b, c) values (2, 3, 4); "
        "insert into iins1 (a, b, c) values (2, 4, 5); "
        "insert into iins1 (a, b, c) values (4, 5, 6); "
        "insert into iins1 (a, b, ex) values ( 3, 5, 10 ); "
        "insert into iins1 (a, b, ex) values ( 6, 7, \"abc\");");
    ASSERT_EQ(batch.statements.size(), 5u);
    for (const auto& s : batch.statements) {
        EXPECT_EQ(s.kind(), StatementKind::insert);
    }
    EXPECT_FALSE(batch.error.has_value());
}

TEST(Batch, BareSemicolonsAreEmptyStatements) {
    // Mid-batch bare ';' is a no-op empty statement (renders nothing); only an
    // empty parse at end-of-input terminates the batch.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch(";; show version;");
    ASSERT_EQ(batch.statements.size(), 3u);
    EXPECT_EQ(batch.statements[0].kind(), StatementKind::empty);
    EXPECT_EQ(batch.statements[1].kind(), StatementKind::empty);
    EXPECT_EQ(batch.statements[2].kind(), StatementKind::show_version);
    EXPECT_FALSE(batch.error.has_value());
}

TEST(Batch, TrailingIncompleteTailIsErrorButPrefixReturned) {
    // "execute what you can": the two complete statements are returned, and the
    // incomplete tail (`drop tab` is not `drop table <name>`) is the error.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch("show version; quit; drop tab");
    ASSERT_EQ(batch.statements.size(), 2u);
    EXPECT_EQ(batch.statements[0].kind(), StatementKind::show_version);
    EXPECT_EQ(batch.statements[1].kind(), StatementKind::quit);
    ASSERT_TRUE(batch.error.has_value());
    EXPECT_EQ(batch.error->code, ddbsh::Errc::parse_error);
}

TEST(Batch, TokenTraceSecondStatementIntact) {
    // The architect's lookahead concern: the first statement's YYACCEPT at ';'
    // must not swallow the second statement's leading token. Verify the second
    // statement parses with its full content (a DROP TABLE with the right name
    // and NOWAIT), not a truncated fragment.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch("show version; drop table nowait mytable;");
    ASSERT_EQ(batch.statements.size(), 2u);
    EXPECT_EQ(batch.statements[0].kind(), StatementKind::show_version);
    ASSERT_EQ(batch.statements[1].kind(), StatementKind::drop_table);
    const auto& drop = std::get<ddbsh::engine::DropTableCmd>(batch.statements[1].ast()->command);
    EXPECT_EQ(drop.table, "mytable");
    EXPECT_TRUE(drop.nowait);
}

TEST(Batch, ExplainWithinBatch) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch("explain select * from t; show version;");
    ASSERT_EQ(batch.statements.size(), 2u);
    EXPECT_EQ(batch.statements[0].kind(), StatementKind::select_statement);
    EXPECT_TRUE(batch.statements[0].explain());
    EXPECT_EQ(batch.statements[1].kind(), StatementKind::show_version);
    EXPECT_FALSE(batch.error.has_value());
}

TEST(Batch, ErrorLocationStaysContinuousAcrossStatements) {
    // The line/column cursor is continuous across statements (not reset per
    // statement), so a syntax error in a later statement reports the position
    // in the whole batch — matching the original's run-loop error locations.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto batch = session.parse_batch("show version; show bogus;");
    ASSERT_EQ(batch.statements.size(), 1u);
    ASSERT_TRUE(batch.error.has_value());
    // The error string carries a [line, col] prefix past the first statement.
    EXPECT_NE(batch.error->message.find("["), std::string::npos);
}

}  // namespace
