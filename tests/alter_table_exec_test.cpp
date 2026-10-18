// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// alter_table_exec_test.cpp — 2e-1 ALTER TABLE SET (non-GSI) execution tests.
//
// Exercises the UpdateTable path offline via FakeDDBClient:
//   * each SET clause (billing/throughput, warm, sse, stream, table class,
//     deletion protection) builds the right UpdateTableIn, carrying ONLY the
//     field(s) the clause provided (the rest stay nullopt so the adapter omits
//     them — arch §8, D-2e-2);
//   * a successful UpdateTable renders the ALTER status token;
//   * EXPLAIN renders one `UpdateTable(...)` block with NO trailing status
//     token (contrast CREATE's "CREATE") — D-2e-11; and
//   * an UpdateTable failure propagates as an Error.

#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::BillingMode;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::SseType;
using ddbsh::StreamViewType;
using ddbsh::TableClass;
using ddbsh::UpdateTableIn;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

TEST(AlterTableExec, ProvisionedBillingSetsModeAndThroughput) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set billing mode provisioned ( 200 rcu, 100 wcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");

    ASSERT_EQ(client.update_table_requests().size(), 1u);
    const auto& req = client.update_table_requests()[0];
    EXPECT_EQ(req.table_name, "t");
    ASSERT_TRUE(req.billing.has_value());
    EXPECT_EQ(req.billing->mode, BillingMode::provisioned);
    EXPECT_EQ(req.billing->rcu, 200);
    EXPECT_EQ(req.billing->wcu, 100);
    EXPECT_FALSE(req.warm.has_value());
    EXPECT_FALSE(req.sse.has_value());
    EXPECT_FALSE(req.stream.has_value());
    EXPECT_FALSE(req.table_class.has_value());
    EXPECT_FALSE(req.deletion_protection.has_value());
}

TEST(AlterTableExec, OnDemandUnlimitedClearsBothDimensions) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set billing mode on demand throughput unlimited;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.billing.has_value());
    EXPECT_EQ(req.billing->mode, BillingMode::pay_per_request);
    ASSERT_TRUE(req.billing->od.max_read_request_units.has_value());
    ASSERT_TRUE(req.billing->od.max_write_request_units.has_value());
    EXPECT_EQ(*req.billing->od.max_read_request_units, -1);
    EXPECT_EQ(*req.billing->od.max_write_request_units, -1);
}

TEST(AlterTableExec, BillingWithWarmThroughputCarriesBoth) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "alter table t set billing mode provisioned ( 10 rcu, 20 wcu ) "
        "warm throughput ( 5 rcu, 6 wcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.billing.has_value());
    ASSERT_TRUE(req.warm.has_value());
    EXPECT_EQ(req.warm->read_units_per_second, 5);
    EXPECT_EQ(req.warm->write_units_per_second, 6);
}

TEST(AlterTableExec, StandaloneWarmThroughputOnly) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set warm throughput ( 7 rcu, 8 wcu );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    EXPECT_FALSE(req.billing.has_value());
    ASSERT_TRUE(req.warm.has_value());
    EXPECT_EQ(req.warm->read_units_per_second, 7);
    EXPECT_EQ(req.warm->write_units_per_second, 8);
}

TEST(AlterTableExec, SseSpecificationCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set sse ( keyid mykey kms );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.sse.has_value());
    EXPECT_TRUE(req.sse->enabled);
    ASSERT_TRUE(req.sse->sse_type.has_value());
    EXPECT_EQ(*req.sse->sse_type, SseType::kms);
    ASSERT_TRUE(req.sse->kms_master_key_id.has_value());
    EXPECT_EQ(*req.sse->kms_master_key_id, "mykey");
}

TEST(AlterTableExec, StreamSpecificationCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set stream ( both images );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.stream.has_value());
    EXPECT_TRUE(req.stream->enabled);
    EXPECT_EQ(req.stream->view_type, StreamViewType::new_and_old_images);
}

TEST(AlterTableExec, TableClassCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set table class standard infrequent access;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.table_class.has_value());
    EXPECT_EQ(*req.table_class, TableClass::standard_infrequent_access);
    // Only table class set; everything else unset.
    EXPECT_FALSE(req.billing.has_value());
    EXPECT_FALSE(req.deletion_protection.has_value());
}

TEST(AlterTableExec, DeletionProtectionDisabledCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set deletion protection disabled;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.deletion_protection.has_value());
    EXPECT_FALSE(*req.deletion_protection);
    EXPECT_FALSE(req.table_class.has_value());
}

TEST(AlterTableExec, DeletionProtectionEnabledCaptured) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set deletion protection enabled;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.deletion_protection.has_value());
    EXPECT_TRUE(*req.deletion_protection);
}

TEST(AlterTableExec, ExplainEmitsUpdateTableBlockWithNoTrailingStatus) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("explain alter table t set deletion protection enabled;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // One UpdateTable block, pretty-formatted; NO trailing ALTER token.
    EXPECT_EQ(render(*out), "UpdateTable({\n   \"stub\":   \"explain\"\n})\n");

    // The serialized request is an UpdateTableIn carrying the parsed field.
    ASSERT_EQ(client.explain_requests().size(), 1u);
    const auto* req = std::get_if<UpdateTableIn>(&client.explain_requests()[0]);
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->table_name, "t");
    ASSERT_TRUE(req->deletion_protection.has_value());
    EXPECT_TRUE(*req->deletion_protection);
    // EXPLAIN never issues the mutation.
    EXPECT_TRUE(client.update_table_requests().empty());
}

TEST(AlterTableExec, UpdateTableFailurePropagates) {
    ddbsh::test::FakeDDBClient client;
    client.set_update_table_error(
        Error{Errc::service_error, "Update Table failed. X. Y. boom", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set deletion protection enabled;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "Update Table failed. X. Y. boom");
}

}  // namespace
