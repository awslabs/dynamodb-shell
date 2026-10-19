// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// return_values_ratelimit_test.cpp — 2g RETURN VALUES + WITH RATELIMIT +
// RETURN CONSUMED CAPACITY tests.
//
// Three concerns, all offline:
//   * RETURN VALUES rendering, both shapes: INSERT/REPLACE emit `op: {item}`
//     (the status token REPLACED by the verb+payload form, insert.cpp), while
//     bulk/single UPDATE/UPSERT/DELETE emit each returned item BEFORE the
//     `(n read, m modified, k ccf)` accounting line (update.cpp/delete.cpp
//     unsorted order; -s-stable). Pins the ddbsh-rv-* golden shapes.
//   * The token-bucket RateLimiter math via an INJECTED clock/sleep (zero real
//     wall-clock delay): rate-0 disables, deficit->sleep decision, and the
//     cap-at-rate (a long idle banks at most `rate` credit).
//   * Executor ratelimit WIRING: the metered write/read requests carry
//     ReturnConsumedCapacity=TOTAL and output is unchanged; plus the
//     RETURN CONSUMED CAPACITY `SELECT (...)` line (incl. INDEXES) and the
//     ratelimit/RCC mutual-exclusion parse error.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "execute/rate_limiter.hpp"
#include "fake_ddb_client.hpp"

namespace {

using ddbsh::ConsumedCapacity;
using ddbsh::DescribeTableOut;
using ddbsh::ExecOutcome;
using ddbsh::ItemRow;
using ddbsh::ReturnConsumedCapacity;
using ddbsh::TableKeySchema;
using ddbsh::Value;
using ddbsh::engine::RateLimiter;

DescribeTableOut table_pk(const std::string& name, const std::string& pk,
                          const std::string& rk = "") {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{pk, rk};
    return t;
}

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

// Build an item row from (name, number-literal) pairs.
ItemRow num_row(std::vector<std::pair<std::string, std::string>> fields) {
    ItemRow row;
    for (auto& [name, n] : fields) {
        row.attributes.emplace_back(name, Value::number_value(n));
    }
    return row;
}

ConsumedCapacity total_cc(double units) {
    ConsumedCapacity cc;
    cc.capacity_units = units;
    return cc;
}

// ======================= RETURN VALUES rendering ============================

TEST(ReturnValues, ReplaceAllOldRendersVerbColonItem) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_put_attributes(num_row({{"a", "3"}, {"b", "4"}, {"c", "5"}}));
    ddbsh::Session session = make_session(client);

    auto out = session.run("replace into t (a, b, c) values (3, 4, 5) return values all old;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // The status token becomes `REPLACE: {item}` (ddbsh-rv-test-1 shape).
    EXPECT_EQ(render(*out), "REPLACE: {a: 3, b: 4, c: 5}\n");
    // The request carried ReturnValues (adapter sets it iff != none).
    ASSERT_EQ(client.put_requests().size(), 1u);
    EXPECT_EQ(client.put_requests()[0].return_values, ddbsh::ReturnValue::all_old);
}

TEST(ReturnValues, ReplaceReturnRequestedButNoAttributesRendersTrailingColon) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    // No canned attributes: the service returned nothing (e.g. ALL_OLD on a new
    // key). The original still prints `REPLACE: ` + show_item("") = "REPLACE: ".
    ddbsh::Session session = make_session(client);

    auto out = session.run("replace into t (a) values (9) return values all old;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "REPLACE: \n");
}

TEST(ReturnValues, InsertNeverCarriesReturnValues) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_put_attributes(num_row({{"a", "1"}}));  // ignored: INSERT forces none
    ddbsh::Session session = make_session(client);

    auto out = session.run("insert into t (a) values (1);");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "INSERT\n");
    EXPECT_EQ(client.put_requests()[0].return_values, ddbsh::ReturnValue::none);
}

TEST(ReturnValues, UpdateSingleFullKeyRendersItemThenStatus) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_update_attributes(num_row({{"a", "0"}, {"y", "4"}}));
    ddbsh::Session session = make_session(client);

    auto out = session.run("update t set y = 4 where a = 0 return values all new;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // Item first, then the accounting line (update.cpp unsorted order).
    EXPECT_EQ(render(*out), "{a: 0, y: 4}\nUPDATE (0 read, 1 modified, 0 ccf)\n");
    ASSERT_EQ(client.update_requests().size(), 1u);
    EXPECT_EQ(client.update_requests()[0].return_values, ddbsh::ReturnValue::all_new);
}

TEST(ReturnValues, UpsertSingleFullKeyRendersItemThenStatus) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_update_attributes(num_row({{"a", "4"}, {"y", "6"}, {"z", "3"}}));
    ddbsh::Session session = make_session(client);

    auto out = session.run("upsert t set y = 6, z = 3 where a = 4 return values all old;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "{a: 4, y: 6, z: 3}\nUPSERT (0 read, 1 modified, 0 ccf)\n");
}

TEST(ReturnValues, DeleteSingleFullKeyRendersItemThenStatus) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_delete_attributes(num_row({{"a", "4"}, {"b", "5"}, {"c", "6"}}));
    ddbsh::Session session = make_session(client);

    auto out = session.run("delete from t where a = 4 return values all old;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "{a: 4, b: 5, c: 6}\nDELETE (0 read, 1 modified, 0 ccf)\n");
    ASSERT_EQ(client.delete_item_requests().size(), 1u);
    EXPECT_EQ(client.delete_item_requests()[0].return_values, ddbsh::ReturnValue::all_old);
}

TEST(ReturnValues, BulkUpdateRendersEachItemThenStatus) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_items({num_row({{"a", "1"}}), num_row({{"a", "2"}})});
    client.set_update_attributes(num_row({{"a", "1"}, {"x", "4"}}));
    ddbsh::Session session = make_session(client);

    // No WHERE -> scan-all -> two enumerated items, each written.
    auto out = session.run("update t set x = 4 return values all old;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "{a: 1, x: 4}\n{a: 1, x: 4}\nUPDATE (2 read, 2 modified, 0 ccf)\n");
}

TEST(ReturnValues, BulkDeleteRendersEachItemThenStatus) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_items({num_row({{"a", "1"}}), num_row({{"a", "2"}}), num_row({{"a", "3"}})});
    client.set_delete_attributes(num_row({{"a", "1"}, {"b", "2"}}));
    ddbsh::Session session = make_session(client);

    auto out = session.run("delete from t return values all old;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out),
              "{a: 1, b: 2}\n{a: 1, b: 2}\n{a: 1, b: 2}\nDELETE (3 read, 3 modified, 0 ccf)\n");
}

TEST(ReturnValues, NoReturnValuesEmitsPlainTokensAndStatus) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_update_attributes(num_row({{"a", "0"}, {"y", "4"}}));  // present, but not requested
    ddbsh::Session session = make_session(client);

    // Without RETURN VALUES the returned-item payload is NOT rendered.
    auto out = session.run("update t set y = 4 where a = 0;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "UPDATE (0 read, 1 modified, 0 ccf)\n");
}

// ======================= RateLimiter token-bucket math ======================

// A deterministic clock/sleep pair: the clock reads a shared seconds counter;
// the injected sleep records each requested duration AND advances the clock by
// it (so the refresh after a sleep sees exactly the slept time elapse). No real
// wall-clock delay.
struct FakeTime {
    std::shared_ptr<double> now = std::make_shared<double>(0.0);
    std::shared_ptr<std::vector<double>> sleeps = std::make_shared<std::vector<double>>();

    RateLimiter::Clock clock() const {
        auto n = now;
        return [n]() { return std::chrono::duration<double>(*n); };
    }
    RateLimiter::Sleep sleep() const {
        auto n = now;
        auto s = sleeps;
        return [n, s](std::chrono::duration<double> d) {
            s->push_back(d.count());
            *n += d.count();  // the sleep actually elapses this much time
        };
    }
};

TEST(RateLimiterMath, ZeroRateDisablesAndNeverSleeps) {
    FakeTime ft;
    RateLimiter limiter(0.0, 0.0, ft.clock(), ft.sleep());
    limiter.consume_reads(total_cc(100.0));   // drives read tokens deeply negative
    limiter.consume_writes(total_cc(100.0));  // and write tokens
    limiter.readlimit();
    limiter.writelimit();
    EXPECT_TRUE(ft.sleeps->empty());  // rate 0 => no throttling, no divide-by-zero
}

TEST(RateLimiterMath, PositiveTokensDoNotSleep) {
    FakeTime ft;
    RateLimiter limiter(10.0, 10.0, ft.clock(), ft.sleep());
    // Tokens start at 0 (>= 0), so both limits return immediately.
    limiter.readlimit();
    limiter.writelimit();
    EXPECT_TRUE(ft.sleeps->empty());
}

TEST(RateLimiterMath, DeficitSleepsExactlyDeficitOverRate) {
    FakeTime ft;
    RateLimiter limiter(10.0, 10.0, ft.clock(), ft.sleep());
    limiter.consume_reads(total_cc(10.0));  // read tokens = -10
    limiter.readlimit();                    // deficit 10 / rate 10 => 1.0s, then refilled to 0
    ASSERT_EQ(ft.sleeps->size(), 1u);
    EXPECT_DOUBLE_EQ((*ft.sleeps)[0], 1.0);
}

TEST(RateLimiterMath, WriteBucketIsIndependentOfReadBucket) {
    FakeTime ft;
    RateLimiter limiter(1.0, 5.0, ft.clock(), ft.sleep());
    limiter.consume_writes(total_cc(5.0));  // write tokens = -5
    limiter.writelimit();                   // 5 / 5 => 1.0s
    ASSERT_EQ(ft.sleeps->size(), 1u);
    EXPECT_DOUBLE_EQ((*ft.sleeps)[0], 1.0);
    // The read bucket was never touched, so a read limit does not sleep.
    limiter.readlimit();
    EXPECT_EQ(ft.sleeps->size(), 1u);
}

TEST(RateLimiterMath, BucketCapsAtRateAcrossLongIdle) {
    FakeTime ft;
    RateLimiter limiter(5.0, 0.0, ft.clock(), ft.sleep());
    // Idle 100s: an uncapped bucket would bank 500 credit; the cap pins it at 5.
    *ft.now = 100.0;
    limiter.readlimit();  // refills to the cap (5), >= 0 => no sleep
    EXPECT_TRUE(ft.sleeps->empty());
    // Now consume 10: if the bank had been 500 this would stay positive and not
    // throttle; because it capped at 5, tokens go to -5 and we must sleep 1s.
    limiter.consume_reads(total_cc(10.0));
    limiter.readlimit();
    ASSERT_EQ(ft.sleeps->size(), 1u);
    EXPECT_DOUBLE_EQ((*ft.sleeps)[0], 1.0);
}

TEST(RateLimiterMath, MissingCapacityContributesZero) {
    FakeTime ft;
    RateLimiter limiter(10.0, 10.0, ft.clock(), ft.sleep());
    limiter.consume_reads(std::nullopt);         // no cc => no throttle
    limiter.consume_writes(ConsumedCapacity{});  // present but no CapacityUnits => 0
    limiter.readlimit();
    limiter.writelimit();
    EXPECT_TRUE(ft.sleeps->empty());
}

// ======================= Executor ratelimit WIRING ==========================
// These assert request-shape (ReturnConsumedCapacity=TOTAL) + unchanged output.
// They leave the canned write/read consumed capacity at its default (none) so
// the executor's own real-clock RateLimiter never actually sleeps (tokens stay
// >= 0). The timing itself is covered by the RateLimiter math tests above and
// the live ddbsh-basic golden family.

TEST(RatelimitWiring, InsertForcesReturnConsumedCapacityTotal) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("insert into t (a) values (1) with ratelimit ( 1 wcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "INSERT\n");  // output unchanged by ratelimit
    ASSERT_EQ(client.put_requests().size(), 1u);
    EXPECT_EQ(client.put_requests()[0].return_consumed_capacity, ReturnConsumedCapacity::total);
}

TEST(RatelimitWiring, UpdateForcesReturnConsumedCapacityTotal) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("update t set y = 1 where a = 0 with ratelimit ( 1 wcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "UPDATE (0 read, 1 modified, 0 ccf)\n");
    ASSERT_EQ(client.update_requests().size(), 1u);
    EXPECT_EQ(client.update_requests()[0].return_consumed_capacity, ReturnConsumedCapacity::total);
}

TEST(RatelimitWiring, DeleteForcesReturnConsumedCapacityTotal) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("delete from t where a = 4 with ratelimit ( 1 wcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "DELETE (0 read, 1 modified, 0 ccf)\n");
    ASSERT_EQ(client.delete_item_requests().size(), 1u);
    EXPECT_EQ(client.delete_item_requests()[0].return_consumed_capacity,
              ReturnConsumedCapacity::total);
}

TEST(RatelimitWiring, SelectRatelimitPaginatesWithoutDisplayingCapacity) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    // Two pages so the pagination loop (and readlimit before the next page)
    // runs; default (no) consumed capacity => the executor's limiter never
    // sleeps.
    client.set_pages({{num_row({{"a", "1"}})}, {num_row({{"a", "2"}})}});
    ddbsh::Session session = make_session(client);

    auto out = session.run("select * from t with ratelimit ( 1 rcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(client.read_calls(), 2);            // both pages fetched
    EXPECT_EQ(render(*out), "{a: 1}\n{a: 2}\n");  // no SELECT (...) line (RCC not requested)
}

// ======================= RETURN CONSUMED CAPACITY ===========================

TEST(ReturnConsumedCapacityDisplay, IndexesRendersSelectLine) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    client.set_items({num_row({{"a", "1"}})});
    client.set_consumed_capacity(
        ConsumedCapacity{std::string{"t"}, 5.0, std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    // INDEXES displays identically to TOTAL (the request carries the
    // distinction; the display does not — select.cpp display_consumed_capacity).
    auto out = session.run("select * from t return consumed capacity indexes;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "{a: 1}\nSELECT (t, 5, 0, 0)\n");
}

TEST(ReturnConsumedCapacityDisplay, RatelimitAndRccAreMutuallyExclusive) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "a"));
    ddbsh::Session session = make_session(client);

    auto out =
        session.run("select * from t return consumed capacity total with ratelimit ( 1 rcu );");
    ASSERT_FALSE(out.has_value());
    EXPECT_NE(out.error().message.find(
                  "You cannot specify both a ratelimit and a return_consumed_capacity."),
              std::string::npos);
}

}  // namespace
