// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// create_table_exec_test.cpp — W5 part 2b CREATE TABLE execution tests.
//
// Exercises the non-EXPLAIN CREATE TABLE path offline via FakeDDBClient:
//   * CREATE issues CreateTable and (unless NOWAIT) waits for the table to
//     become ACTIVE; it renders the CREATE status token.
//   * The parsed table definition is faithfully copied into the CreateTableIn
//     request (name, attributes, key schema, billing, index set).
//   * A nullopt billing (no billing clause) is forwarded as nullopt so the
//     adapter applies the on-demand default (arch §8).
//   * IF NOT EXISTS probes the table first: on an existing table it skips the
//     CreateTable call but still renders CREATE; on a missing table it creates.
//   * A CreateTable failure (e.g. ResourceInUse "Table already exists")
//     propagates as an Error with the service message.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::BillingMode;
using ddbsh::DescribeTableOut;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::ScalarType;
using ddbsh::TableKeySchema;

DescribeTableOut table_pk(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{{"a"}, {}};
    return t;
}

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

TEST(CreateTableExec, ProvisionedCreatesAndWaits) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( a number ) primary key ( a hash ) "
        "billing mode provisioned ( 200 rcu, 100 wcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");

    ASSERT_EQ(client.create_requests().size(), 1u);
    const auto& req = client.create_requests()[0];
    EXPECT_EQ(req.table_name, "t");
    ASSERT_EQ(req.attribute_definitions.size(), 1u);
    EXPECT_EQ(req.attribute_definitions[0].name, "a");
    EXPECT_EQ(req.attribute_definitions[0].type, ScalarType::n);
    EXPECT_EQ(req.key_schema.hash, (std::vector<std::string>{"a"}));
    EXPECT_TRUE(req.key_schema.range.empty());
    ASSERT_TRUE(req.billing.has_value());
    EXPECT_EQ(req.billing->mode, BillingMode::provisioned);
    EXPECT_EQ(req.billing->rcu, 200);
    EXPECT_EQ(req.billing->wcu, 100);

    ASSERT_EQ(client.waited_active().size(), 1u);
    EXPECT_EQ(client.waited_active()[0], "t");
}

TEST(CreateTableExec, NoBillingClauseForwardsNulloptForOnDemandDefault) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");

    ASSERT_EQ(client.create_requests().size(), 1u);
    // nullopt billing => the adapter applies the on-demand default (arch §8).
    EXPECT_FALSE(client.create_requests()[0].billing.has_value());
}

TEST(CreateTableExec, HashRangeKeySchemaCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out =
        session.run("create table t ( a number, b string ) primary key ( a hash, b range );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    const auto& req = client.create_requests()[0];
    EXPECT_EQ(req.key_schema.hash, (std::vector<std::string>{"a"}));
    EXPECT_EQ(req.key_schema.range, (std::vector<std::string>{"b"}));
}

TEST(CreateTableExec, NowaitSkipsWaiter) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table nowait t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");
    EXPECT_EQ(client.create_requests().size(), 1u);
    EXPECT_TRUE(client.waited_active().empty());
}

TEST(CreateTableExec, IfNotExistsOnExistingSkipsCreateButStillRendersCreate) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t"));  // table already exists
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table if not exists t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");
    EXPECT_TRUE(client.create_requests().empty());  // no CreateTable issued
    EXPECT_TRUE(client.waited_active().empty());
}

TEST(CreateTableExec, IfNotExistsOnMissingCreates) {
    ddbsh::test::FakeDDBClient client;  // table not registered
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table if not exists t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");
    EXPECT_EQ(client.create_requests().size(), 1u);
}

TEST(CreateTableExec, CreateFailurePropagatesServiceMessage) {
    ddbsh::test::FakeDDBClient client;
    client.set_create_error(Error{Errc::service_error, "Table already exists: t",
                                  std::string{"ResourceInUseException"}, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table t ( a number ) primary key ( a hash );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "Table already exists: t");
    EXPECT_TRUE(client.waited_active().empty());  // no wait after a failed create
}

TEST(CreateTableExec, GsiAndLsiCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( a number, b string, c number ) primary key ( a hash, b range ) "
        "gsi ( myindex on ( c hash ) projecting all ) "
        "lsi ( myidx on ( a hash, c range ) projecting keys only );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    const auto& req = client.create_requests()[0];
    ASSERT_EQ(req.gsis.size(), 1u);
    EXPECT_EQ(req.gsis[0].name, "myindex");
    EXPECT_EQ(req.gsis[0].key_schema.hash, (std::vector<std::string>{"c"}));
    ASSERT_EQ(req.lsis.size(), 1u);
    EXPECT_EQ(req.lsis[0].name, "myidx");
    EXPECT_EQ(req.lsis[0].key_schema.hash, (std::vector<std::string>{"a"}));
    EXPECT_EQ(req.lsis[0].key_schema.range, (std::vector<std::string>{"c"}));
}

TEST(CreateTableExec, CompositeGsiCaptured) {
    // phase 4a: a composite GSI key is accepted and threaded into the request.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( a string, b string, c string, d string, x string ) "
        "primary key ( x hash ) "
        "gsi ( g on ( (a, b) hash, (c, d) range ) projecting all );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_EQ(client.create_requests().size(), 1u);
    const auto& req = client.create_requests()[0];
    ASSERT_EQ(req.gsis.size(), 1u);
    EXPECT_EQ(req.gsis[0].key_schema.hash, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(req.gsis[0].key_schema.range, (std::vector<std::string>{"c", "d"}));
}

TEST(CreateTableExec, CompositeBasePrimaryKeyRejected) {
    // A composite key on a base-table primary key is rejected client-side
    // (GSI-only; ratified D-4a-1/-8). No CreateTable request is issued.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table t ( a string, b string ) primary key ( (a, b) hash );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message,
              "Composite keys are supported only on GSIs. A table primary key must have "
              "one partition key and an optional sort key.");
    EXPECT_TRUE(client.create_requests().empty());
}

TEST(CreateTableExec, CompositeLsiRejected) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( x string, a string, b string ) primary key ( x hash ) "
        "lsi ( l1 on ( x hash, (a, b) range ) projecting all );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message,
              "Composite keys are supported only on GSIs. LSI l1 must have "
              "one partition key and an optional sort key.");
    EXPECT_TRUE(client.create_requests().empty());
}

TEST(CreateTableExec, BasePrimaryKeyMissingHashRejected) {
    // The generalized key_schema grammar accepts a role list with no HASH role;
    // reject it client-side with a clear error rather than sending an empty HASH
    // list to the service (reviewer S2, phase 4a).
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("create table t ( a string ) primary key ( a range );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "A table primary key must specify a partition (HASH) key.");
    EXPECT_TRUE(client.create_requests().empty());
}

TEST(CreateTableExec, LsiMissingHashRejected) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( x string, a string ) primary key ( x hash ) "
        "lsi ( l1 on ( a range ) projecting all );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "LSI l1 must specify a partition (HASH) key.");
    EXPECT_TRUE(client.create_requests().empty());
}

TEST(CreateTableExec, GsiMissingHashRejected) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( x string, a string ) primary key ( x hash ) "
        "gsi ( g on ( a range ) projecting all );");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "GSI g must specify a partition (HASH) key.");
    EXPECT_TRUE(client.create_requests().empty());
}

// ---- Composite GSI 4-attribute limit (phase 4b backlog, G1) ----------------

TEST(CreateTableExec, CompositeGsiExceedingFourAttributesRejected) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table t ( x string, a string, b string, c string, d string, e string ) "
        "primary key ( x hash ) "
        "gsi ( g on ( (a, b, c, d, e) hash ) projecting all );");
    ASSERT_FALSE(out.has_value());
    EXPECT_NE(out.error().message.find("4-attribute limit"), std::string::npos);
    EXPECT_TRUE(client.create_requests().empty());
}

// ---- Vector index (phase 4b) ----------------------------------------------

TEST(CreateTableExec, VsiCaptured) {
    // vsi ( name on vector-attr dimensions n distance fn projecting all ). The
    // vector attribute (embedding) is NOT declared in the attribute list
    // (D-4b-8). The definition is threaded into CreateTableIn.vector_indexes.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table docs ( id string ) primary key ( id hash ) "
        "vsi ( embidx on embedding dimensions 1536 distance cosine projecting all );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_EQ(client.create_requests().size(), 1u);
    const auto& req = client.create_requests()[0];
    ASSERT_EQ(req.vector_indexes.size(), 1u);
    EXPECT_EQ(req.vector_indexes[0].index_name, "embidx");
    EXPECT_EQ(req.vector_indexes[0].vector_attr, "embedding");
    EXPECT_EQ(req.vector_indexes[0].dimensions, 1536);
    EXPECT_EQ(req.vector_indexes[0].distance, ddbsh::VectorDistance::cosine);
    EXPECT_EQ(req.vector_indexes[0].projection.type, ddbsh::ProjectionType::all);
}

TEST(CreateTableExec, VsiVectorKeywordSynonym) {
    // `vector ( ... )` is a synonym for `vsi ( ... )` (D-4b-9).
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table docs ( id string ) primary key ( id hash ) "
        "vector ( embidx on embedding dimensions 768 distance euclidean projecting all );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_EQ(client.create_requests().size(), 1u);
    const auto& req = client.create_requests()[0];
    ASSERT_EQ(req.vector_indexes.size(), 1u);
    EXPECT_EQ(req.vector_indexes[0].dimensions, 768);
    EXPECT_EQ(req.vector_indexes[0].distance, ddbsh::VectorDistance::euclidean);
}

TEST(CreateTableExec, VsiVectorIndexKeywordSynonym) {
    // `vector index ( ... )` is a synonym for `vsi ( ... )` (D-4b-9).
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table docs ( id string ) primary key ( id hash ) "
        "vector index ( embidx on embedding dimensions 384 distance dot_product "
        "projecting keys only );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_EQ(client.create_requests().size(), 1u);
    const auto& req = client.create_requests()[0];
    ASSERT_EQ(req.vector_indexes.size(), 1u);
    EXPECT_EQ(req.vector_indexes[0].distance, ddbsh::VectorDistance::dot_product);
    EXPECT_EQ(req.vector_indexes[0].projection.type, ddbsh::ProjectionType::keys_only);
}

TEST(CreateTableExec, VsiSearchSchemaAndInclude) {
    // SEARCH KEY ( attr hash, attr filter ) + PROJECTING INCLUDE; the search
    // and projected attrs ARE declared (D-4b-8); the vector attr is not.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table docs ( id string, tenant string, category string, title string ) "
        "primary key ( id hash ) "
        "vsi ( embidx on embedding dimensions 1024 distance cosine "
        "search key ( tenant hash, category filter ) projecting include ( title ) );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    const auto& vi = client.create_requests()[0].vector_indexes[0];
    ASSERT_EQ(vi.search_schema.size(), 2u);
    EXPECT_EQ(vi.search_schema[0].attr, "tenant");
    EXPECT_EQ(vi.search_schema[0].kind, ddbsh::SearchSchemaKind::hash);
    EXPECT_EQ(vi.search_schema[1].attr, "category");
    EXPECT_EQ(vi.search_schema[1].kind, ddbsh::SearchSchemaKind::inline_filter);
    EXPECT_EQ(vi.projection.type, ddbsh::ProjectionType::include);
    EXPECT_EQ(vi.projection.non_key_attributes, (std::vector<std::string>{"title"}));
}

TEST(CreateTableExec, VsiVectorAttrDeclaredRejected) {
    // D-4b-8: the vector attribute must NOT appear in the attribute-type list.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table docs ( id string, embedding string ) primary key ( id hash ) "
        "vsi ( embidx on embedding dimensions 16 distance cosine projecting all );");
    ASSERT_FALSE(out.has_value());
    EXPECT_NE(out.error().message.find("must not be declared"), std::string::npos);
    EXPECT_TRUE(client.create_requests().empty());
}

TEST(CreateTableExec, VsiSearchKeyUndeclaredRejected) {
    // A SEARCH KEY attribute must be declared in the attribute-type list.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "create table docs ( id string ) primary key ( id hash ) "
        "vsi ( embidx on embedding dimensions 16 distance cosine "
        "search key ( tenant hash ) projecting all );");
    ASSERT_FALSE(out.has_value());
    EXPECT_NE(out.error().message.find("must be declared"), std::string::npos);
    EXPECT_TRUE(client.create_requests().empty());
}

}  // namespace
