// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// prepared_test.cpp — native unit tests for prepared / parameterized statements
// and set/binary binding (D-P5-2 / D-P5-3, Approach H).
//
// Two offline backends drive the embedding Connection::prepare surface:
//   * ddbsh::test::FakeDDBClient supplies a key schema and records the request
//     the engine builds, so an EXPLAIN-EQUIVALENCE check can prove a `?`-bound
//     statement produces the IDENTICAL request as the equivalent literal (D6).
//   * ddbsh::testing::MemoryClient is a real in-memory store, so a prepare →
//     bind → execute → re-execute round-trip (including set/binary binds that
//     have no ddbsh literal syntax) is verified end-to-end with no AWS account.
// The real-DDB round-trip and byte-exact EXPLAIN-equivalence live in the live
// API test (connection_live_test.cpp).

#include "ddbsh/connection.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <variant>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/testing/memory_client.hpp"
#include "fake_ddb_client.hpp"

namespace {

using ddbsh::Connection;
using ddbsh::SessionConfig;
using ddbsh::Value;
using ddbsh::test::FakeDDBClient;
using ddbsh::testing::MemoryClient;

// Structural equality for two Values (recursive; sets compared order-
// insensitively, matching DynamoDB's unordered-set semantics — F1).
bool same_value(const Value& a, const Value& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case Value::Type::S:
        case Value::Type::N:
        case Value::Type::B:
            return a.s == b.s;
        case Value::Type::BOOL:
            return a.boolean == b.boolean;
        case Value::Type::NUL:
            return true;
        case Value::Type::SS:
        case Value::Type::NS:
        case Value::Type::BS: {
            std::vector<std::string> x = a.set;
            std::vector<std::string> y = b.set;
            std::sort(x.begin(), x.end());
            std::sort(y.begin(), y.end());
            return x == y;
        }
        case Value::Type::L: {
            if (a.list.size() != b.list.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.list.size(); ++i) {
                if (!same_value(a.list[i], b.list[i])) {
                    return false;
                }
            }
            return true;
        }
        case Value::Type::M: {
            if (a.map.size() != b.map.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.map.size(); ++i) {
                if (a.map[i].first != b.map[i].first ||
                    !same_value(a.map[i].second, b.map[i].second)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

bool same_vmap(const ddbsh::ValueMap& a, const ddbsh::ValueMap& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first || !same_value(a[i].second, b[i].second)) {
            return false;
        }
    }
    return true;
}

// ---- EXPLAIN-equivalence: a `?`-bound request == the literal request (D6) --

TEST(Prepared, ExplainGetItemRequestEqualsLiteral) {
    ddbsh::DescribeTableOut schema;
    schema.table_name = "t";
    schema.key_schema = ddbsh::test::make_key_schema("id");

    FakeDDBClient bound_client;
    bound_client.set_table(schema);
    Connection bound(SessionConfig{"us-east-1", ""}, bound_client);
    auto prep = bound.prepare("explain select * from t where id = ?;");
    ASSERT_TRUE(prep.has_value());
    prep->bind(1, std::string("m1"));
    ASSERT_TRUE(bound.exec(*prep).has_value());

    FakeDDBClient lit_client;
    lit_client.set_table(schema);
    Connection lit(SessionConfig{"us-east-1", ""}, lit_client);
    ASSERT_TRUE(lit.exec("explain select * from t where id = \"m1\";").has_value());

    ASSERT_EQ(bound_client.explain_requests().size(), 1u);
    ASSERT_EQ(lit_client.explain_requests().size(), 1u);
    const auto* bg = std::get_if<ddbsh::GetItemIn>(&bound_client.explain_requests()[0]);
    const auto* lg = std::get_if<ddbsh::GetItemIn>(&lit_client.explain_requests()[0]);
    ASSERT_NE(bg, nullptr);  // hash-only key + `id =` reduces to GetItem
    ASSERT_NE(lg, nullptr);
    EXPECT_EQ(bg->table_name, lg->table_name);
    EXPECT_TRUE(same_vmap(bg->key, lg->key));  // the bound value == the literal
}

TEST(Prepared, ExplainQueryFilterRequestEqualsLiteral) {
    ddbsh::DescribeTableOut schema;
    schema.table_name = "t";
    schema.key_schema = ddbsh::test::make_key_schema("id");

    FakeDDBClient bound_client;
    bound_client.set_table(schema);
    Connection bound(SessionConfig{"us-east-1", ""}, bound_client);
    auto prep = bound.prepare("explain select * from t where id = ? and n = ?;");
    ASSERT_TRUE(prep.has_value());
    prep->bind(1, std::string("m1")).bind(2, 7LL);
    ASSERT_TRUE(bound.exec(*prep).has_value());

    FakeDDBClient lit_client;
    lit_client.set_table(schema);
    Connection lit(SessionConfig{"us-east-1", ""}, lit_client);
    ASSERT_TRUE(lit.exec("explain select * from t where id = \"m1\" and n = 7;").has_value());

    ASSERT_EQ(bound_client.explain_requests().size(), 1u);
    ASSERT_EQ(lit_client.explain_requests().size(), 1u);
    const auto* bq = std::get_if<ddbsh::QueryIn>(&bound_client.explain_requests()[0]);
    const auto* lq = std::get_if<ddbsh::QueryIn>(&lit_client.explain_requests()[0]);
    ASSERT_NE(bq, nullptr);  // hash `=` + non-key filter -> Query
    ASSERT_NE(lq, nullptr);
    EXPECT_EQ(bq->key_condition_expression, lq->key_condition_expression);
    EXPECT_EQ(bq->filter_expression, lq->filter_expression);
    EXPECT_TRUE(same_vmap(bq->expression_attribute_values, lq->expression_attribute_values));
}

// ---- Offline round-trips over a real in-memory store -----------------------

void seed_movies(Connection& db) {
    ASSERT_TRUE(db.exec("create table movies ( id string, yr number ) "
                        "primary key ( id hash, yr range );")
                    .has_value());
    ASSERT_TRUE(db.exec("insert into movies ( id, yr, title ) "
                        "values ( \"m1\", 2010, \"Inception\" );")
                    .has_value());
    ASSERT_TRUE(db.exec("insert into movies ( id, yr, title ) "
                        "values ( \"m2\", 2020, \"Tenet\" );")
                    .has_value());
}

TEST(Prepared, RoundTripMatchesLiteral) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    seed_movies(db);

    auto prep = db.prepare("select * from movies where id = ? and yr = ?;");
    ASSERT_TRUE(prep.has_value());
    prep->bind(1, std::string("m1")).bind(2, 2010LL);
    auto bound = db.query(*prep);
    ASSERT_TRUE(bound.has_value());

    auto literal = db.query("select * from movies where id = \"m1\" and yr = 2010;");
    ASSERT_TRUE(literal.has_value());

    ASSERT_EQ(bound->size(), literal->size());
    ASSERT_EQ(bound->size(), 1u);
    EXPECT_EQ((*bound)[0].get_string("title"), (*literal)[0].get_string("title"));
    EXPECT_EQ((*bound)[0].get_string("title"), "Inception");
}

TEST(Prepared, BindStringSetRoundTrip) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());

    // A set has NO ddbsh literal syntax; the only way to store one is a typed
    // bind (D-P5-3).
    auto ins = db.prepare("insert into t ( id, labels ) values ( ?, ? );");
    ASSERT_TRUE(ins.has_value());
    ins->bind(1, std::string("a")).bind(2, Value::string_set({"red", "green", "blue"}));
    ASSERT_TRUE(db.exec(*ins).has_value());

    auto rows = db.query("select * from t where id = \"a\";");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    const auto* labels = (*rows)[0].cell("labels").as_string_set();
    ASSERT_NE(labels, nullptr);
    std::vector<std::string> got = *labels;
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, (std::vector<std::string>{"blue", "green", "red"}));
}

TEST(Prepared, BindBinaryRoundTrip) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());

    auto ins = db.prepare("insert into t ( id, blob ) values ( ?, ? );");
    ASSERT_TRUE(ins.has_value());
    // "aGVsbG8=" is base64("hello"); binary has no literal syntax either.
    ins->bind(1, std::string("a")).bind(2, Value::binary_value("aGVsbG8="));
    ASSERT_TRUE(db.exec(*ins).has_value());

    auto rows = db.query("select * from t where id = \"a\";");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    ddbsh::Cell blob = (*rows)[0].cell("blob");
    ASSERT_FALSE(blob.missing());
    EXPECT_EQ(blob.type(), Value::Type::B);
    EXPECT_EQ(blob.value().s, "aGVsbG8=");
}

TEST(Prepared, BoundSetMatchesStoredSetOrderInsensitively) {
    // F1: DynamoDB sets are unordered, so a bound {"c","a","b"} must match a
    // stored {"a","b","c"} in a filter equality.
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());

    auto ins = db.prepare("insert into t ( id, labels ) values ( ?, ? );");
    ASSERT_TRUE(ins.has_value());
    ins->bind(1, std::string("a")).bind(2, Value::string_set({"a", "b", "c"}));
    ASSERT_TRUE(db.exec(*ins).has_value());

    auto match = db.prepare("select * from t where id = ? and labels = ?;");
    ASSERT_TRUE(match.has_value());
    match->bind(1, std::string("a")).bind(2, Value::string_set({"c", "a", "b"}));  // reordered
    auto rows = db.query(*match);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 1u);  // matches despite element order

    auto miss = db.prepare("select * from t where id = ? and labels = ?;");
    ASSERT_TRUE(miss.has_value());
    miss->bind(1, std::string("a")).bind(2, Value::string_set({"a", "b"}));  // different set
    auto none = db.query(*miss);
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->size(), 0u);
}

TEST(Prepared, MarkerNestedInList) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());

    // A marker may sit inside a list literal.
    auto ins = db.prepare("insert into t ( id, nums ) values ( ?, [ ?, ?, 99 ] );");
    ASSERT_TRUE(ins.has_value());
    EXPECT_EQ(ins->parameter_count(), 3);
    ins->bind(1, std::string("a")).bind(2, 1LL).bind(3, 2LL);
    ASSERT_TRUE(db.exec(*ins).has_value());

    auto rows = db.query("select * from t where id = \"a\";");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    const auto* nums = (*rows)[0].cell("nums").as_list();
    ASSERT_NE(nums, nullptr);
    ASSERT_EQ(nums->size(), 3u);
    EXPECT_EQ((*nums)[0].s, "1");
    EXPECT_EQ((*nums)[1].s, "2");
    EXPECT_EQ((*nums)[2].s, "99");
}

TEST(Prepared, PreparedTransactionWithMarkers) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());

    // Markers inside a transaction body (one prepared statement).
    auto txn = db.prepare(
        "begin; "
        "insert into t ( id, v ) values ( ?, ? ); "
        "insert into t ( id, v ) values ( ?, ? ); "
        "commit;");
    ASSERT_TRUE(txn.has_value());
    EXPECT_EQ(txn->parameter_count(), 4);
    txn->bind(1, std::string("x")).bind(2, 1LL).bind(3, std::string("y")).bind(4, 2LL);
    ASSERT_TRUE(db.exec(*txn).has_value());

    auto rows = db.query("select * from t;");
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2u);
}

// D-P13-13: `?` is a CONTAINS operand. Binding NULL, a number, a bool and a
// string builds the same Scan request as the literal operand.
TEST(Prepared, ContainsOperandBindsLikeLiteral) {
    ddbsh::DescribeTableOut schema;
    schema.table_name = "t";
    schema.key_schema = ddbsh::test::make_key_schema("id");

    struct Case {
        Value bound;
        const char* literal;
    };
    const Case cases[] = {
        {Value::null_value(), "NULL"},
        {Value::number_value("5"), "5"},
        {Value::bool_value(true), "TRUE"},
        {Value::string_value("x y"), "\"x y\""},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.literal);
        FakeDDBClient bound_client;
        bound_client.set_table(schema);
        Connection bound(SessionConfig{"us-east-1", ""}, bound_client);
        auto prep = bound.prepare("explain select * from t where contains(l, ?);");
        ASSERT_TRUE(prep.has_value());
        EXPECT_EQ(prep->parameter_count(), 1);
        prep->bind(1, c.bound);
        ASSERT_TRUE(bound.exec(*prep).has_value());

        FakeDDBClient lit_client;
        lit_client.set_table(schema);
        Connection lit(SessionConfig{"us-east-1", ""}, lit_client);
        ASSERT_TRUE(
            lit.exec(std::string("explain select * from t where contains(l, ") + c.literal + ");")
                .has_value());

        ASSERT_EQ(bound_client.explain_requests().size(), 1u);
        ASSERT_EQ(lit_client.explain_requests().size(), 1u);
        const auto* bs = std::get_if<ddbsh::ScanIn>(&bound_client.explain_requests()[0]);
        const auto* ls = std::get_if<ddbsh::ScanIn>(&lit_client.explain_requests()[0]);
        ASSERT_NE(bs, nullptr);
        ASSERT_NE(ls, nullptr);
        EXPECT_EQ(bs->filter_expression, ls->filter_expression);
        EXPECT_TRUE(same_vmap(bs->expression_attribute_values, ls->expression_attribute_values));
    }
}

// A `?` CONTAINS operand on the unprepared path is unbound, not a syntax error.
TEST(Prepared, ContainsOperandUnboundOnExec) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    ASSERT_TRUE(db.exec("create table t ( id string ) primary key ( id hash );").has_value());
    auto r = db.exec("select * from t where contains(l, ?);");
    if (r.has_value()) {
        r->render_all();
        ASSERT_TRUE(r->error().has_value());
        EXPECT_EQ(r->error()->code, ddbsh::Errc::unbound_parameter);
    } else {
        EXPECT_EQ(r.error().code, ddbsh::Errc::unbound_parameter);
    }
}

TEST(Prepared, ParseErrorAtPrepare) {
    MemoryClient client;
    Connection db(SessionConfig{"us-east-1", ""}, client);
    auto prep = db.prepare("selct * from t;");  // misspelled
    ASSERT_FALSE(prep.has_value());
    EXPECT_EQ(prep.error().code, ddbsh::Errc::parse_error);
}

// ---- A bound sort-key not-equal re-plans per execution (P15, D-P15-8) -------

TEST(Prepared, NotEqualSplitReplansFromTheBoundValue) {
    ddbsh::DescribeTableOut schema;
    schema.table_name = "t";
    schema.key_schema = ddbsh::TableKeySchema{{"pk"}, {"sk"}};
    schema.attribute_definitions = {ddbsh::AttributeDef{"pk", ddbsh::ScalarType::n},
                                    ddbsh::AttributeDef{"sk", ddbsh::ScalarType::n}};
    FakeDDBClient client;
    client.set_table(schema);
    Connection db(SessionConfig{"us-east-1", ""}, client);
    auto prep = db.prepare("explain select * from t where pk = ? and sk <> ?;");
    ASSERT_TRUE(prep.has_value());

    // (1, 5): two Queries, both carrying 5.
    prep->bind(1, 1LL).bind(2, 5LL);
    ASSERT_TRUE(db.exec(*prep).has_value());
    ASSERT_EQ(client.explain_requests().size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* q = std::get_if<ddbsh::QueryIn>(&client.explain_requests()[i]);
        ASSERT_NE(q, nullptr);
        ASSERT_EQ(q->expression_attribute_values.size(), 2u);
        EXPECT_EQ(q->expression_attribute_values[1].second.s, "5");
    }

    // (1, 3): re-planned with 3.
    prep->bind(2, 3LL);
    ASSERT_TRUE(db.exec(*prep).has_value());
    ASSERT_EQ(client.explain_requests().size(), 4u);
    for (std::size_t i = 2; i < 4; ++i) {
        const auto* q = std::get_if<ddbsh::QueryIn>(&client.explain_requests()[i]);
        ASSERT_NE(q, nullptr);
        EXPECT_EQ(q->expression_attribute_values[1].second.s, "3");
    }

    // (1, "x") on a Number sort key: the type rule keeps one Scan.
    prep->bind(2, std::string("x"));
    ASSERT_TRUE(db.exec(*prep).has_value());
    ASSERT_EQ(client.explain_requests().size(), 5u);
    EXPECT_NE(std::get_if<ddbsh::ScanIn>(&client.explain_requests()[4]), nullptr);
}

}  // namespace
