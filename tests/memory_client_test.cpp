// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// memory_client_test.cpp — offline unit tests for the phase-5a MemoryClient
// extensions (D-P5-4): FilterExpression evaluation, opt-in bounded paging,
// transactions, and the minimal control-plane state round-trip.
//
// These exercise the new surfaces the way an embedder would — through a
// ddbsh::Connection driven against a genuine ddbsh::testing::MemoryClient (no
// AWS, no credentials, no network). The engine builds the serialized filter /
// key-condition / projection and the alias maps; MemoryClient interprets them.
// Per D-P5-A8 this offline coverage is ADDITIONAL to the real-DDB gate, never a
// substitute.

#include "ddbsh/connection.hpp"

#include <string>

#include <gtest/gtest.h>

#include "ddbsh/testing/memory_client.hpp"

namespace {

using ddbsh::Connection;
using ddbsh::ResultKind;
using ddbsh::SessionConfig;
using ddbsh::testing::MemoryClient;

// A hash-only table `t ( id )` seeded with rows carrying an `n` number and a
// `tag` string, for filter coverage.
void seed_filter_table(Connection& db) {
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    ASSERT_TRUE(
        db.exec("insert into t ( id, n, tag ) values ( \"a\", 1, \"alpha\" );").has_value());
    ASSERT_TRUE(db.exec("insert into t ( id, n, tag ) values ( \"b\", 5, \"beta\" );").has_value());
    ASSERT_TRUE(
        db.exec("insert into t ( id, n, tag ) values ( \"c\", 9, \"gamma\" );").has_value());
}

// ---- FilterExpression evaluation (scan + query) ----------------------------

TEST(MemoryClientFilter, ScanNumericComparison) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    auto rows = db.query("select * from t where n > 4;");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2u);  // b (5), c (9)
    for (const ddbsh::Row& r : *rows) {
        EXPECT_GT(r.get_int("n").value_or(0), 4);
    }
}

TEST(MemoryClientFilter, ScanBetween) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    auto rows = db.query("select * from t where n between 2 and 8;");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);  // only b (5)
    EXPECT_EQ((*rows)[0].get_string("id"), "b");
}

TEST(MemoryClientFilter, ScanBeginsWith) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    auto rows = db.query("select * from t where begins_with(tag, \"be\");");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].get_string("id"), "b");
}

TEST(MemoryClientFilter, ScanContains) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    auto rows = db.query("select * from t where contains(tag, \"amm\");");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].get_string("id"), "c");  // "gamma"
}

TEST(MemoryClientFilter, ScanIn) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    auto rows = db.query("select * from t where n in ( 1, 9 );");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2u);  // a (1), c (9)
}

TEST(MemoryClientFilter, ScanAttributeExists) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);
    ASSERT_TRUE(db.exec("insert into t ( id, extra ) values ( \"d\", 1 );").has_value());

    auto rows = db.query("select * from t where attribute_exists(extra);");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].get_string("id"), "d");
}

TEST(MemoryClientFilter, ScanAttributeNotExists) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);
    ASSERT_TRUE(db.exec("insert into t ( id, extra ) values ( \"d\", 1 );").has_value());

    auto rows = db.query("select * from t where attribute_not_exists(extra);");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 3u);  // a, b, c lack `extra`
}

TEST(MemoryClientFilter, ScanAttributeType) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    auto rows = db.query("select * from t where attribute_type(n, number);");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 3u);  // every row's n is a number

    auto none = db.query("select * from t where attribute_type(n, string);");
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->size(), 0u);
}

TEST(MemoryClientFilter, ScanOr) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    auto rows = db.query("select * from t where n = 1 or n = 9;");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2u);
}

TEST(MemoryClientFilter, ScanSizeOfString) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    // "alpha"/"gamma" are 5 chars, "beta" is 4.
    auto rows = db.query("select * from t where size(tag) = 4;");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].get_string("id"), "b");
}

TEST(MemoryClientFilter, QueryHashPlusFilter) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table m ( id string, yr number ) "
                        "primary key ( id hash, yr range );")
                    .has_value());
    ASSERT_TRUE(db.exec("insert into m ( id, yr, r ) values ( \"m1\", 2000, 3 );").has_value());
    ASSERT_TRUE(db.exec("insert into m ( id, yr, r ) values ( \"m1\", 2010, 7 );").has_value());

    // Hash equality (key condition) + a non-key predicate (filter).
    auto rows = db.query("select * from m where id = \"m1\" and r > 5;");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].get_int("yr"), 2010);
}

// ---- Bounded paging (opt-in) -----------------------------------------------

TEST(MemoryClientPaging, PagedScanMatchesUnpagedBytes) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_filter_table(db);

    // Unpaged: the whole table in one page.
    auto full = db.exec("select * from t;");
    ASSERT_TRUE(full.has_value());
    std::string unpaged = full->render_all();

    // Paged one item at a time: the engine's executor follows the
    // LastEvaluatedKey cursor and aggregates every page. Output (under the CLI
    // sort the goldens use) is the same set of rows.
    client.set_page_size(1);
    auto paged = db.exec("select * from t;");
    ASSERT_TRUE(paged.has_value());
    EXPECT_EQ(paged->render_all(), unpaged);
    // Three rows total regardless of page size.
    auto rows = db.query("select * from t;");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 3u);
}

TEST(MemoryClientPaging, PagedQueryCrossesBoundaries) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table m ( id string, yr number ) "
                        "primary key ( id hash, yr range );")
                    .has_value());
    for (int i = 0; i < 5; ++i) {
        std::string stmt =
            "insert into m ( id, yr ) values ( \"m1\", " + std::to_string(2000 + i) + " );";
        ASSERT_TRUE(db.exec(stmt).has_value());
    }

    client.set_page_size(2);  // forces 3 pages (2 + 2 + 1)
    auto rows = db.query("select * from m where id = \"m1\";");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 5u);
}

// ---- Transactions (Tier 2) -------------------------------------------------

TEST(MemoryClientTxn, WriteTransactionCommits) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());

    auto r = db.exec(
        "begin; "
        "insert into t ( id, v ) values ( \"x\", 1 ); "
        "insert into t ( id, v ) values ( \"y\", 2 ); "
        "commit;");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->error(), std::nullopt);

    auto rows = db.query("select * from t;");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2u);
}

TEST(MemoryClientTxn, WriteTransactionConditionFailsAtomically) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    ASSERT_TRUE(db.exec("insert into t ( id ) values ( \"x\" );").has_value());

    // One INSERT (attribute_not_exists guard) collides with the existing "x";
    // the whole transaction must roll back — "y" must NOT be created.
    auto r = db.exec(
        "begin; "
        "insert into t ( id ) values ( \"y\" ); "
        "insert into t ( id ) values ( \"x\" ); "
        "commit;");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->error().has_value());

    auto rows = db.query("select * from t where id = \"y\";");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 0u);  // atomic rollback: y not written
}

TEST(MemoryClientTxn, ReadTransactionReturnsItems) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    ASSERT_TRUE(db.exec("insert into t ( id, v ) values ( \"x\", 1 );").has_value());
    ASSERT_TRUE(db.exec("insert into t ( id, v ) values ( \"y\", 2 );").has_value());

    auto r = db.exec(
        "begin; "
        "select * from t where id = \"x\"; "
        "select * from t where id = \"y\"; "
        "commit;");
    ASSERT_TRUE(r.has_value());
    ddbsh::Results& res = *r;
    EXPECT_EQ(res.error(), std::nullopt);

    // A read transaction (TransactGetItems) surfaces every inner GetItem in one
    // rows result set, in request order (x then y).
    ASSERT_EQ(res.kind(), ResultKind::rows);
    ASSERT_TRUE(res.next_row());
    EXPECT_EQ(res.column("id").as_string(), "x");
    ASSERT_TRUE(res.next_row());
    EXPECT_EQ(res.column("id").as_string(), "y");
    EXPECT_FALSE(res.next_row());
    EXPECT_FALSE(res.next_result());
}

// ---- Control-plane state round-trip (Tier 2) -------------------------------

TEST(MemoryClientControlPlane, TtlRoundTrips) {
    MemoryClient client;
    ddbsh::CreateTableIn ct;
    ct.table_name = "t";
    ct.key_schema.hash = {"id"};
    ASSERT_TRUE(client.create_table(ct).has_value());

    ddbsh::DescribeTimeToLiveIn in{"t"};
    auto before = client.describe_time_to_live(in);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->status, "DISABLED");

    ddbsh::UpdateTimeToLiveIn up;
    up.table_name = "t";
    up.enabled = true;
    up.attribute_name = "expires";
    ASSERT_TRUE(client.update_time_to_live(up).has_value());

    auto after = client.describe_time_to_live(in);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, "ENABLED");
    EXPECT_EQ(after->attribute_name, "expires");
}

TEST(MemoryClientControlPlane, PitrRoundTrips) {
    MemoryClient client;
    ddbsh::CreateTableIn ct;
    ct.table_name = "t";
    ct.key_schema.hash = {"id"};
    ASSERT_TRUE(client.create_table(ct).has_value());

    ddbsh::UpdateContinuousBackupsIn up;
    up.table_name = "t";
    up.enabled = true;
    ASSERT_TRUE(client.update_continuous_backups(up).has_value());

    ddbsh::DescribeContinuousBackupsIn in{"t"};
    auto after = client.describe_continuous_backups(in);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->pitr_status, "ENABLED");
}

TEST(MemoryClientControlPlane, DeletionProtectionRoundTrips) {
    MemoryClient client;
    ddbsh::CreateTableIn ct;
    ct.table_name = "t";
    ct.key_schema.hash = {"id"};
    ASSERT_TRUE(client.create_table(ct).has_value());

    ddbsh::UpdateTableIn up;
    up.table_name = "t";
    up.deletion_protection = true;
    ASSERT_TRUE(client.update_table(up).has_value());

    ddbsh::DescribeTableIn in{"t"};
    auto desc = client.describe_table(in);
    ASSERT_TRUE(desc.has_value());
    EXPECT_TRUE(desc->deletion_protection_enabled);
}

}  // namespace
