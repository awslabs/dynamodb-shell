// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// describe_exec_test.cpp — W5 part 2c-ii DESCRIBE execution tests.
//
// Exercises the non-EXPLAIN DESCRIBE report offline via FakeDDBClient, pinning
// the multi-section layout byte-for-byte against the original describe.cpp
// printf sequence (arch §7): basic info (incl. the double-space "Attributes:"
// quirk), billing/throughput, warm throughput, PITR (DescribeContinuousBackups),
// per-GSI/LSI detail (incl. the LSI "all on one line" quirk and the On Demand
// "mirrors table" branch), stream, table class, SSE, and replica auto-scaling.
// Best-effort sections (PITR, auto-scaling) are skipped on failure. No in-scope
// golden drives DESCRIBE this phase (the describe goldens are interleaved with
// ALTER/PITR, deferred to 2c), so these native tests are the format contract.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::AttributeDef;
using ddbsh::ContinuousBackupsOut;
using ddbsh::DescribeTableOut;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::Projection;
using ddbsh::ProjectionType;
using ddbsh::ReplicaAutoScalingDescription;
using ddbsh::ScalarType;
using ddbsh::SecondaryIndexSchema;
using ddbsh::TableKeySchema;
using ddbsh::TableReplicaAutoScalingOut;
using ddbsh::WarmThroughputDescription;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

// A minimal provisioned table (hash+range, two attributes), no indexes, no
// warm throughput, PITR disabled, no replicas — the common base case.
DescribeTableOut base_provisioned(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.table_status = "ACTIVE";
    t.key_schema = TableKeySchema{{"pk"}, {"rk"}};
    t.attribute_definitions = {AttributeDef{"pk", ScalarType::n},
                               AttributeDef{"rk", ScalarType::s}};
    t.creation_date_time = "2026-10-13T00:00:00Z";
    t.table_arn = "arn:t";
    t.table_id = "id-1";
    t.table_size_bytes = 1024;
    t.item_count = 7;
    t.deletion_protection_enabled = false;
    t.billing_mode_summary = "PROVISIONED";
    t.provisioned_throughput_set = true;
    t.provisioned_read_capacity_units = 10;
    t.provisioned_write_capacity_units = 20;
    t.table_class_summary = "STANDARD";
    return t;
}

TEST(Describe, ProvisionedBasicReport) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(base_provisioned("t1"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe t1;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out),
              "Name: t1 (ACTIVE)\n"
              "Key: HASH pk, RANGE rk\n"
              "Attributes:  pk, N,  rk, S\n"
              "Created at: 2026-10-13T00:00:00Z\n"
              "Table ARN: arn:t\n"
              "Table ID: id-1\n"
              "Table size (bytes): 1024\n"
              "Item Count: 7\n"
              "Deletion Protection: Disabled\n"
              "Billing Mode: Provisioned (10 RCU, 20 WCU)\n"
              "PITR is Disabled.\n"
              "GSI: None\n"
              "LSI: None\n"
              "VSI: None\n"
              "Stream: Disabled\n"
              "Table Class: STANDARD\n"
              "SSE: Not set\n");
}

TEST(Describe, OnDemandWithThroughputAndWarm) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t;
    t.table_name = "od";
    t.table_status = "ACTIVE";
    t.key_schema = TableKeySchema{{"a"}, {}};
    t.attribute_definitions = {AttributeDef{"a", ScalarType::n}};
    t.creation_date_time = "2026-10-13T00:00:00Z";
    t.table_arn = "arn:od";
    t.table_id = "id-od";
    t.billing_mode_summary = "PAY_PER_REQUEST";
    t.on_demand_throughput_set = true;
    t.on_demand_max_read_request_units = 10;
    t.on_demand_max_write_request_units = 200;
    t.warm_throughput = WarmThroughputDescription{13000, 4000, "ACTIVE"};
    t.table_class_summary = "STANDARD";
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe od;");
    ASSERT_TRUE(out.has_value());
    const std::string text = render(*out);
    EXPECT_NE(text.find("Billing Mode: On Demand\n"), std::string::npos);
    EXPECT_NE(text.find("On Demand Throughput: (10 RCU, 200 WCU)\n"), std::string::npos);
    EXPECT_NE(text.find("Warm Throughput: (13000 RCU, 4000 WCU, Status ACTIVE)\n"),
              std::string::npos);
}

TEST(Describe, GsiProvisionedWithIncludeProjection) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t = base_provisioned("g");
    SecondaryIndexSchema gsi;
    gsi.index_name = "gsipka";
    gsi.key_schema = TableKeySchema{{"a"}, {}};
    gsi.projection = Projection{ProjectionType::include, {"x", "y"}};
    gsi.projection_set = true;
    gsi.index_status = "ACTIVE";
    gsi.backfilling = false;
    gsi.provisioned_read_capacity_units = 5;
    gsi.provisioned_write_capacity_units = 6;
    t.global_secondary_indexes = {gsi};
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe g;");
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(render(*out).find("GSI gsipka: ( HASH a ),  Provisioned (5 RCU, 6 WCU), "
                                "Projecting (INCLUDE) (x, y), Status: ACTIVE, Backfilling: NO\n"),
              std::string::npos);
}

TEST(Describe, GsiOnDemandMirrorsTableWithWarm) {
    // When the table is On Demand, each GSI prints "On Demand (mirrors table)"
    // plus its on-demand throughput (describe.cpp __show_gsi) and warm status
    // via the IndexStatus mapper.
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t;
    t.table_name = "god";
    t.table_status = "ACTIVE";
    t.key_schema = TableKeySchema{{"pk"}, {}};
    t.attribute_definitions = {AttributeDef{"pk", ScalarType::n}, AttributeDef{"a", ScalarType::n}};
    t.creation_date_time = "2026-10-13T00:00:00Z";
    t.table_arn = "arn:god";
    t.table_id = "id-god";
    t.billing_mode_summary = "PAY_PER_REQUEST";
    t.table_class_summary = "STANDARD";
    SecondaryIndexSchema gsi;
    gsi.index_name = "gsipka";
    gsi.key_schema = TableKeySchema{{"a"}, {}};
    gsi.projection = Projection{ProjectionType::all, {}};
    gsi.projection_set = true;
    gsi.index_status = "ACTIVE";
    gsi.on_demand_throughput_set = true;
    gsi.on_demand_max_read_request_units = 200;
    gsi.on_demand_max_write_request_units = 300;
    gsi.warm_throughput = WarmThroughputDescription{12000, 4000, "UPDATING"};
    t.global_secondary_indexes = {gsi};
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe god;");
    ASSERT_TRUE(out.has_value());
    const std::string text = render(*out);
    EXPECT_NE(
        text.find("GSI gsipka: ( HASH a ), Billing Mode: On Demand (mirrors table), "
                  "Throughput (200 RCU, 300 WCU), Projecting (ALL), Status: ACTIVE, "
                  "Backfilling: NO, Warm Throughput: (12000 RCU, 4000 WCU, Status UPDATING)\n"),
        std::string::npos);
}

TEST(Describe, MultipleLsisOnOneLine) {
    // The original prints the "\n" AFTER the LSI loop, so every LSI entry lands
    // on a single line (describe.cpp __show_lsi). Preserve that quirk.
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t = base_provisioned("l");
    t.local_secondary_indexes = {
        SecondaryIndexSchema{.index_name = "l1", .key_schema = TableKeySchema{{"pk"}, {"c1"}}},
        SecondaryIndexSchema{.index_name = "l2", .key_schema = TableKeySchema{{"pk"}, {"c2"}}},
    };
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe l;");
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(render(*out).find("LSI l1: ( HASH pk, RANGE c1 ), LSI l2: ( HASH pk, RANGE c2 ), \n"),
              std::string::npos);
}

TEST(Describe, PitrEnabledShowsWindow) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(base_provisioned("p"));
    client.set_continuous_backups(
        "p", ContinuousBackupsOut{"ENABLED", "2026-10-01T00:00:00Z", "2026-10-13T00:00:00Z"});
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe p;");
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(
        render(*out).find("PITR is Enabled: [2026-10-01T00:00:00Z to 2026-10-13T00:00:00Z]\n"),
        std::string::npos);
}

TEST(Describe, PitrFailureSkipsSection) {
    // A DescribeContinuousBackups failure skips the PITR line entirely (the
    // original's IsSuccess() guard) — not an error, not a line.
    ddbsh::test::FakeDDBClient client;
    client.set_table(base_provisioned("p"));
    client.set_continuous_backups_error(
        "p", Error{Errc::service_error, "pitr boom", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe p;");
    ASSERT_TRUE(out.has_value());
    const std::string text = render(*out);
    EXPECT_EQ(text.find("PITR"), std::string::npos);
    // The report still completes (GSI/LSI/Stream/... sections present).
    EXPECT_NE(text.find("GSI: None\n"), std::string::npos);
}

TEST(Describe, StreamSseClassAndReplicas) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t = base_provisioned("s");
    t.stream_view_type = "NEW_AND_OLD_IMAGES";
    t.sse_status = "ENABLED";
    t.table_class_summary = "STANDARD_INFREQUENT_ACCESS";
    client.set_table(t);
    client.set_auto_scaling(
        "s", TableReplicaAutoScalingOut{{ReplicaAutoScalingDescription{"us-west-2", "ACTIVE"},
                                         ReplicaAutoScalingDescription{"eu-west-1", "CREATING"}}});
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe s;");
    ASSERT_TRUE(out.has_value());
    const std::string text = render(*out);
    EXPECT_NE(text.find("Stream: NEW_AND_OLD_IMAGES\n"), std::string::npos);
    EXPECT_NE(text.find("Table Class: STANDARD_INFREQUENT_ACCESS\n"), std::string::npos);
    EXPECT_NE(text.find("SSE: (ENABLED)\n"), std::string::npos);
    EXPECT_NE(text.find("Replica Region: us-west-2 (Status: ACTIVE)\n"), std::string::npos);
    EXPECT_NE(text.find("Replica Region: eu-west-1 (Status: CREATING)\n"), std::string::npos);
}

TEST(Describe, ProvisionedFallbackWithoutBillingSummary) {
    // No BillingModeSummary but ProvisionedThroughput present => the original's
    // fallback "Billing Mode: Provisioned (...)" branch.
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t = base_provisioned("f");
    t.billing_mode_summary = std::nullopt;
    t.provisioned_throughput_set = true;
    t.provisioned_read_capacity_units = 3;
    t.provisioned_write_capacity_units = 4;
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe f;");
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(render(*out).find("Billing Mode: Provisioned (3 RCU, 4 WCU)\n"), std::string::npos);
}

TEST(Describe, MissingTableIsError) {
    ddbsh::test::FakeDDBClient client;
    // No set_table => describe_table returns no_such_table.
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe ghost;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::no_such_table);
}

// ---- Vector index (phase 4b, D-4b-10) --------------------------------------

TEST(Describe, VectorIndexSection) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut t = base_provisioned("docs");
    ddbsh::VectorIndexSchema vi;
    vi.index_name = "embidx";
    vi.vector_attr = "embedding";
    vi.dimensions = 1536;
    vi.distance = ddbsh::VectorDistance::cosine;
    vi.search_schema = {{"category", ddbsh::SearchSchemaKind::inline_filter}};
    vi.projection_set = true;
    vi.projection = Projection{ProjectionType::all, {}};
    vi.index_status = "ACTIVE";
    t.vector_indexes = {vi};
    client.set_table(t);
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe docs;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_NE(render(*out).find("VSI embidx: Vector Attribute: embedding, Dimensions: 1536, "
                                "Distance: COSINE, Search Schema: ( category (INLINE_FILTER) ), "
                                "Projecting (ALL), Status: ACTIVE\n"),
              std::string::npos)
        << render(*out);
}

}  // namespace
