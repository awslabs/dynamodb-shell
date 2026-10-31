// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// connection_live_test.cpp — real-DynamoDB functional tests for the embedding
// API (P4d, human acceptance criterion 1).
//
// Exercises ddbsh::aws::open + the Connection/Results/Row/Cell surface against
// REAL DynamoDB (us-east-1) through the standard ~/.aws credential/region
// chain. In-memory tests (connection_api_test.cpp) are ADDITIONAL; this is the
// mandatory real-backend coverage.
//
// WARNING — LIVE and SPENDS: creates + drops a real on-demand table and
// writes/reads real items. The table is uniquified per run and dropped on
// every exit path (RAII), so nothing is stranded. Built + registered only
// under DDBSH_TEST_LIVE_SAMPLE (the live gate); never part of the default
// offline suite.

#include "ddbsh/aws/database.hpp"

#include <gtest/gtest.h>

#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <string>

namespace {

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string(fallback);
}

std::string unique_table() {
    return "ddbsh_live_api_test_" + std::to_string(static_cast<long>(getpid())) + "_" +
           std::to_string(static_cast<long long>(std::time(nullptr)));
}

// Opens a real Database, creates a uniquified table, runs the full embedding-API
// surface against it, and self-cleans. One test to minimize table churn/cost.
TEST(ConnectionLive, FullRoundTripAgainstRealDynamoDB) {
    const std::string region = env_or("AWS_REGION", env_or("AWS_DEFAULT_REGION", "us-east-1").c_str());

    auto db = ddbsh::aws::open(region);
    ASSERT_TRUE(db.has_value()) << (db.has_value() ? "" : db.error().message);

    const std::string table = unique_table();
    bool created = false;

    // RAII teardown: drop on every exit path once created.
    struct Teardown {
        ddbsh::aws::Database* db;
        const std::string& table;
        const bool& created;
        ~Teardown() {
            if (created) {
                (void)db->exec("drop table " + table + " nowait;");
            }
        }
    } teardown{&*db, table, created};

    // CREATE (on-demand, waits ACTIVE) -> exercises create_table + waiter.
    {
        auto r = db->exec("create table " + table +
                          " ( id string, year number ) primary key ( id hash, year range )"
                          " billing mode on demand;");
        ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
        EXPECT_EQ(r->status_token(), "CREATE");
        ASSERT_FALSE(r->error().has_value());
        created = true;
    }

    // INSERT three rows.
    for (const char* row : {R"(( "movie", 2010, "Inception" ))",
                            R"(( "movie", 2014, "Interstellar" ))",
                            R"(( "show", 2016, "Westworld" ))"}) {
        auto r = db->exec("insert into " + table + " ( id, year, title ) values " + row + ";");
        ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
        EXPECT_EQ(r->status_token(), "INSERT");
        ASSERT_FALSE(r->error().has_value());
    }

    // GetItem — full key.
    {
        auto rows = db->query("select * from " + table + " where id = \"movie\" and year = 2010;");
        ASSERT_TRUE(rows.has_value()) << (rows.has_value() ? "" : rows.error().message);
        ASSERT_EQ(rows->size(), 1u);
        EXPECT_EQ((*rows)[0].get_string("title"), "Inception");
        EXPECT_EQ((*rows)[0].get_int("year"), 2010);
    }

    // Query — hash only (two movie rows).
    {
        auto rows = db->query("select * from " + table + " where id = \"movie\";");
        ASSERT_TRUE(rows.has_value()) << (rows.has_value() ? "" : rows.error().message);
        EXPECT_EQ(rows->size(), 2u);
        for (const ddbsh::Row& r : *rows) {
            EXPECT_EQ(r.get_string("id"), "movie");
        }
    }

    // Scan — whole table (three rows).
    {
        auto rows = db->query("select * from " + table + ";");
        ASSERT_TRUE(rows.has_value()) << (rows.has_value() ? "" : rows.error().message);
        EXPECT_EQ(rows->size(), 3u);
    }

    // Prepared, parameter-bound query (injection-safe path).
    {
        ddbsh::PreparedStatement stmt("select * from " + table +
                                      " where id = ? and year = ?;");
        stmt.bind(1, std::string("show")).bind(2, 2016LL);
        auto rows = db->query(stmt);
        ASSERT_TRUE(rows.has_value()) << (rows.has_value() ? "" : rows.error().message);
        ASSERT_EQ(rows->size(), 1u);
        EXPECT_EQ((*rows)[0].get_string("title"), "Westworld");
    }

    // DROP — explicit, waits gone. Then dismiss teardown's NOWAIT drop.
    {
        auto r = db->exec("drop table " + table + ";");
        ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
        EXPECT_EQ(r->status_token(), "DROP");
        created = false;  // dropped; teardown is now a no-op
    }
}

}  // namespace
