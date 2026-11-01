// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// connection_api_test.cpp — native unit tests for the embedding API (P4d).
//
// Drives the ddbsh::Connection / Results / Row / Cell / PreparedStatement
// surface offline against a genuine ddbsh::testing::MemoryClient (no AWS, no
// credentials, no network). Exercises the three access plans (GetItem / Query /
// Scan), the typed null-aware value model, client-side parameter binding,
// multi-statement result-set navigation, the exact-bytes render path, and the
// error/closure paths.

#include "ddbsh/connection.hpp"

#include <gtest/gtest.h>

#include "ddbsh/testing/memory_client.hpp"

namespace {

using ddbsh::Connection;
using ddbsh::ResultKind;
using ddbsh::SessionConfig;
using ddbsh::Value;
using ddbsh::testing::MemoryClient;

// Create a movies table and seed three rows. Returns a connection over the
// shared in-memory client (kept alive by the caller via the client ref).
void seed(Connection& db) {
    ASSERT_TRUE(db.exec("create table movies ( id string, yr number ) "
                        "primary key ( id hash, yr range );")
                    .has_value());
    ASSERT_TRUE(db.exec("insert into movies ( id, yr, title ) "
                        "values ( \"m1\", 2010, \"Inception\" );")
                    .has_value());
    ASSERT_TRUE(db.exec("insert into movies ( id, yr, title ) "
                        "values ( \"m1\", 1999, \"Older\" );")
                    .has_value());
    ASSERT_TRUE(db.exec("insert into movies ( id, yr, title ) "
                        "values ( \"m2\", 2020, \"Tenet\" );")
                    .has_value());
}

TEST(ConnectionApi, GetItemFullKey) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed(db);

    auto rows = db.query("select * from movies where id = \"m1\" and yr = 2010;");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].get_string("title"), "Inception");
    EXPECT_EQ((*rows)[0].get_int("yr"), 2010);
    EXPECT_EQ((*rows)[0].get_string("id"), "m1");
}

TEST(ConnectionApi, QueryHashOnly) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed(db);

    auto rows = db.query("select * from movies where id = \"m1\";");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2u);  // both m1 rows, either sort order
    for (const ddbsh::Row& r : *rows) {
        EXPECT_EQ(r.get_string("id"), "m1");
    }
}

TEST(ConnectionApi, ScanWholeTable) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed(db);

    auto rows = db.query("select * from movies;");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 3u);
}

TEST(ConnectionApi, CellThreeState) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    ASSERT_TRUE(
        db.exec("insert into t ( id, present, nul ) values ( \"a\", \"v\", null );").has_value());

    auto rows = db.query("select * from t where id = \"a\";");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    const ddbsh::Row& row = (*rows)[0];

    ddbsh::Cell present = row.cell("present");
    EXPECT_FALSE(present.missing());
    EXPECT_FALSE(present.is_null());
    EXPECT_EQ(present.as_string(), "v");

    ddbsh::Cell nul = row.cell("nul");
    EXPECT_FALSE(nul.missing());
    EXPECT_TRUE(nul.is_null());
    EXPECT_EQ(nul.as_string(), std::nullopt);

    ddbsh::Cell absent = row.cell("nope");
    EXPECT_TRUE(absent.missing());
    EXPECT_FALSE(absent.is_null());
    EXPECT_EQ(absent.as_string(), std::nullopt);
}

TEST(ConnectionApi, NumericTypedAccessors) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    ASSERT_TRUE(db.exec("insert into t ( id, i, d ) values ( \"a\", 42, 3.5 );").has_value());

    auto rows = db.query("select * from t where id = \"a\";");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    const ddbsh::Row& row = (*rows)[0];

    EXPECT_EQ(row.get_int("i"), 42);
    // as_double must be locale-independent (DynamoDB N always uses '.').
    auto d = row.get_double("d");
    ASSERT_TRUE(d.has_value());
    EXPECT_DOUBLE_EQ(*d, 3.5);
    // A non-integral N is not an int; a plain int N parses as a double too.
    EXPECT_EQ(row.get_int("d"), std::nullopt);
    EXPECT_EQ(row.get_double("i"), 42.0);
}

TEST(ConnectionApi, PreparedPositionalBinding) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed(db);

    ddbsh::PreparedStatement stmt("select * from movies where id = ? and yr = ?;");
    stmt.bind(1, std::string("m2")).bind(2, 2020LL);
    auto rows = db.query(stmt);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].get_string("title"), "Tenet");
}

TEST(ConnectionApi, PreparedNamedBinding) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed(db);

    // The ':' introduces the placeholder; bind by the bare name (no colon).
    ddbsh::PreparedStatement stmt("select * from movies where id = :mid;");
    stmt.bind("mid", std::string("m1"));
    auto rows = db.query(stmt);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2u);
}

TEST(ConnectionApi, PreparedStringEscaping) {
    // A bound string with embedded quotes must round-trip through the lexer's
    // "" doubling, not break out of the literal (injection-safe).
    ddbsh::PreparedStatement stmt("select * from t where s = ?;");
    stmt.bind(1, std::string("a\"b"));
    auto expanded = stmt.expand();
    ASSERT_TRUE(expanded.has_value());
    EXPECT_EQ(*expanded, "select * from t where s = \"a\"\"b\";");
}

TEST(ConnectionApi, PreparedUnboundPlaceholderError) {
    ddbsh::PreparedStatement stmt("select * from t where a = ? and b = ?;");
    stmt.bind(1, 1LL);  // only one of two bound
    auto expanded = stmt.expand();
    ASSERT_FALSE(expanded.has_value());
    EXPECT_EQ(expanded.error().code, ddbsh::Errc::invalid_argument);
}

TEST(ConnectionApi, MultiStatementResultSets) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed(db);

    auto r = db.exec(
        "insert into movies ( id, yr ) values ( \"m3\", 1 ); "
        "select * from movies where id = \"m3\";");
    ASSERT_TRUE(r.has_value());
    ddbsh::Results& res = *r;

    // First result set: the INSERT status token.
    EXPECT_EQ(res.kind(), ResultKind::status);
    EXPECT_EQ(res.status_token(), "INSERT");

    // Second result set: the SELECT rows.
    ASSERT_TRUE(res.next_result());
    EXPECT_EQ(res.kind(), ResultKind::rows);
    ASSERT_TRUE(res.next_row());
    EXPECT_EQ(res.column("id").as_string(), "m3");
    EXPECT_FALSE(res.next_row());

    EXPECT_FALSE(res.next_result());
    EXPECT_EQ(res.error(), std::nullopt);
}

TEST(ConnectionApi, RenderMatchesCliBytes) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    ASSERT_TRUE(db.exec("insert into t ( id, title ) values ( \"a\", \"hello\" );").has_value());

    auto r = db.exec("select * from t where id = \"a\";");
    ASSERT_TRUE(r.has_value());
    // Attributes alpha-sorted, top-level ": " spacing (arch §9).
    EXPECT_EQ(r->render(), "{id: a, title: hello}\n");
}

TEST(ConnectionApi, QueryKindMismatchIsError) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());

    auto rows = db.query("insert into t ( id ) values ( \"a\" );");
    ASSERT_FALSE(rows.has_value());
    EXPECT_EQ(rows.error().code, ddbsh::Errc::invalid_argument);
}

TEST(ConnectionApi, ClosedConnectionRejects) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    db.close();
    auto r = db.exec("select * from t;");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ddbsh::Errc::invalid_argument);
}

TEST(ConnectionApi, ConditionalInsertFailsOnExisting) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    ASSERT_TRUE(db.exec("insert into t ( id ) values ( \"a\" );").has_value());

    // A second INSERT of the same key must fail the attribute_not_exists guard;
    // the engine surfaces the original's "The conditional request failed".
    auto r = db.exec("insert into t ( id ) values ( \"a\" );");
    // INSERT is a single conditional write: the failure is the statement's Error.
    ASSERT_TRUE(r.has_value());  // exec itself succeeds; error rides on the result
    EXPECT_TRUE(r->error().has_value());
    EXPECT_EQ(r->error()->code, ddbsh::Errc::conditional_check_failed);
}

TEST(ConnectionApi, BulkWriteAccounting) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed(db);

    // A hash-only WHERE on a hash+range table cannot reduce to a single
    // UpdateItem, so the engine enumerates the matching rows via Query and
    // issues a per-row UpdateItem — a bulk write. Both m1 rows match.
    auto r = db.exec("update movies set rating = 5 where id = \"m1\";");
    ASSERT_TRUE(r.has_value());
    ddbsh::Results& res = *r;
    ASSERT_EQ(res.kind(), ResultKind::write);
    EXPECT_EQ(res.verb(), ddbsh::WriteVerb::update);
    EXPECT_EQ(res.read(), 2);
    EXPECT_EQ(res.modified(), 2);
    EXPECT_EQ(res.ccf(), 0);
    EXPECT_EQ(res.error(), std::nullopt);

    // The update is observable: both m1 rows now carry rating = 5.
    auto rows = db.query("select * from movies where id = \"m1\";");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2u);
    for (const ddbsh::Row& row : *rows) {
        EXPECT_EQ(row.get_int("rating"), 5);
    }
}

TEST(ConnectionApi, RenderAllConcatenatesResultSets) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());

    // Two statements => two status result sets; render_all concatenates the
    // byte-exact CLI output of each in order.
    auto r = db.exec(
        "insert into t ( id ) values ( \"a\" ); "
        "insert into t ( id ) values ( \"b\" );");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->render_all(), "INSERT\nINSERT\n");
}

TEST(ConnectionApi, ReconnectAndEndpointOverride) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    EXPECT_EQ(db.region(), "us-east-1");
    EXPECT_FALSE(db.endpoint_overridden());

    ASSERT_TRUE(db.reconnect("us-west-2", "http://localhost:8000").has_value());
    EXPECT_EQ(db.region(), "us-west-2");
    EXPECT_TRUE(db.endpoint_overridden());
}

TEST(ConnectionApi, BindRejectsNonPositiveIndex) {
    // A 1-based positional index < 1 is caller misuse; bind() cannot return a
    // Result, so the error is surfaced at expand()/exec() (never swallowed).
    ddbsh::PreparedStatement stmt("select * from t where a = ?;");
    stmt.bind(0, 1LL);
    auto expanded = stmt.expand();
    ASSERT_FALSE(expanded.has_value());
    EXPECT_EQ(expanded.error().code, ddbsh::Errc::invalid_argument);
}

TEST(ConnectionApi, OwningClientConstructor) {
    auto client = std::make_unique<MemoryClient>();
    Connection db(SessionConfig{"us-east-1", ""}, std::move(client));
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    auto rows = db.query("select * from t;");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 0u);
}

}  // namespace
