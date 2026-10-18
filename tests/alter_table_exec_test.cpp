// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// alter_table_exec_test.cpp — ALTER TABLE execution tests (2e-1 SET non-GSI,
// 2e-2 GSI create/update/drop, 2e-3 SET TTL).
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
using ddbsh::GsiUpdate;
using ddbsh::ProjectionType;
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

std::string render_stderr(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render_stderr(outcome);
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

// ---- 2e-2: GSI create / update / drop -------------------------------------

TEST(AlterTableExec, CreateGsiCarriesAttributeDefsAndCreateAction) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "alter table t (b number) create gsi g1 on (b hash) projecting all "
        "warm throughput (12000 rcu, 4000 wcu);");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");

    ASSERT_EQ(client.update_table_requests().size(), 1u);
    const auto& req = client.update_table_requests()[0];
    EXPECT_EQ(req.table_name, "t");
    ASSERT_EQ(req.attribute_definitions.size(), 1u);
    EXPECT_EQ(req.attribute_definitions[0].name, "b");
    EXPECT_EQ(req.attribute_definitions[0].type, ddbsh::ScalarType::n);
    ASSERT_EQ(req.gsi_updates.size(), 1u);
    const auto& gu = req.gsi_updates[0];
    EXPECT_EQ(gu.action, GsiUpdate::Action::create);
    EXPECT_EQ(gu.index_name, "g1");
    ASSERT_TRUE(gu.key_schema.has_value());
    EXPECT_EQ(gu.key_schema->hash, "b");
    EXPECT_FALSE(gu.key_schema->range.has_value());
    ASSERT_TRUE(gu.projection.has_value());
    EXPECT_EQ(gu.projection->type, ProjectionType::all);
    EXPECT_FALSE(gu.billing.has_value());
    ASSERT_TRUE(gu.warm.has_value());
    EXPECT_EQ(gu.warm->read_units_per_second, 12000);
    EXPECT_EQ(gu.warm->write_units_per_second, 4000);
}

TEST(AlterTableExec, CreateGsiWithOnDemandBilling) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "alter table t (b number) create gsi g1 on (b hash) projecting keys only "
        "billing mode on demand throughput (200 rcu, 300 wcu);");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& gu = client.update_table_requests()[0].gsi_updates[0];
    EXPECT_EQ(gu.action, GsiUpdate::Action::create);
    ASSERT_TRUE(gu.projection.has_value());
    EXPECT_EQ(gu.projection->type, ProjectionType::keys_only);
    ASSERT_TRUE(gu.billing.has_value());
    EXPECT_EQ(gu.billing->mode, BillingMode::pay_per_request);
    ASSERT_TRUE(gu.billing->od.max_read_request_units.has_value());
    EXPECT_EQ(*gu.billing->od.max_read_request_units, 200);
    EXPECT_EQ(*gu.billing->od.max_write_request_units, 300);
}

TEST(AlterTableExec, DropGsiCarriesDropAction) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t drop gsi g1;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");

    const auto& req = client.update_table_requests()[0];
    EXPECT_TRUE(req.attribute_definitions.empty());
    ASSERT_EQ(req.gsi_updates.size(), 1u);
    EXPECT_EQ(req.gsi_updates[0].action, GsiUpdate::Action::drop);
    EXPECT_EQ(req.gsi_updates[0].index_name, "g1");
}

TEST(AlterTableExec, UpdateGsiOnDemandUnlimited) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    // icd-28 form: bare UPDATE GSI (no table-level SET billing).
    auto out = session.run(
        "alter table t update gsi (g1 set billing mode on demand throughput unlimited);");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    EXPECT_FALSE(req.billing.has_value());  // no table-level billing
    ASSERT_EQ(req.gsi_updates.size(), 1u);
    const auto& gu = req.gsi_updates[0];
    EXPECT_EQ(gu.action, GsiUpdate::Action::update);
    EXPECT_EQ(gu.index_name, "g1");
    ASSERT_TRUE(gu.billing.has_value());
    EXPECT_EQ(gu.billing->mode, BillingMode::pay_per_request);
    EXPECT_EQ(*gu.billing->od.max_read_request_units, -1);
    EXPECT_EQ(*gu.billing->od.max_write_request_units, -1);
}

TEST(AlterTableExec, UpdateGsiMultipleElementsBillingAndWarm) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "alter table t update gsi (g1 set billing mode on demand throughput (200 rcu, 300 wcu), "
        "g2 set warm throughput (9 rcu, 10 wcu));");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);

    const auto& req = client.update_table_requests()[0];
    ASSERT_EQ(req.gsi_updates.size(), 2u);
    EXPECT_EQ(req.gsi_updates[0].index_name, "g1");
    ASSERT_TRUE(req.gsi_updates[0].billing.has_value());
    EXPECT_FALSE(req.gsi_updates[0].warm.has_value());
    EXPECT_EQ(req.gsi_updates[1].index_name, "g2");
    EXPECT_FALSE(req.gsi_updates[1].billing.has_value());
    ASSERT_TRUE(req.gsi_updates[1].warm.has_value());
    EXPECT_EQ(req.gsi_updates[1].warm->read_units_per_second, 9);
    EXPECT_EQ(req.gsi_updates[1].warm->write_units_per_second, 10);
}

TEST(AlterTableExec, CombinedProvisionedBillingKeepsGsiUpdates) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "alter table t set billing mode provisioned (10 rcu, 20 wcu) "
        "update gsi (g1 set billing mode provisioned (30 rcu, 40 wcu));");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // Provisioned table: GSI updates retained; no diagnostic.
    EXPECT_EQ(render(*out), "ALTER\n");
    EXPECT_EQ(render_stderr(*out), "");

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.billing.has_value());
    EXPECT_EQ(req.billing->mode, BillingMode::provisioned);
    ASSERT_EQ(req.gsi_updates.size(), 1u);
    EXPECT_EQ(req.gsi_updates[0].index_name, "g1");
}

TEST(AlterTableExec, CombinedOnDemandBillingDropsGsiUpdatesWithDiagnostic) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    // D-2e-6 gate: on-demand table implies index throughput; GSI updates dropped
    // and the informational message is surfaced on stderr (unpinned).
    auto out = session.run(
        "alter table t set billing mode on demand "
        "update gsi (g1 set billing mode on demand throughput (200 rcu, 300 wcu));");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");
    EXPECT_EQ(render_stderr(*out),
              "Setting table to on-demand, ignoring specifications for indices.\n");

    const auto& req = client.update_table_requests()[0];
    ASSERT_TRUE(req.billing.has_value());
    EXPECT_EQ(req.billing->mode, BillingMode::pay_per_request);
    EXPECT_TRUE(req.gsi_updates.empty());  // dropped by the gate
}

TEST(AlterTableExec, ExplainCreateGsiEmitsUpdateTableBlockNoTrailingStatus) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out =
        session.run("explain alter table t (b number) create gsi g1 on (b hash) projecting all;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "UpdateTable({\n   \"stub\":   \"explain\"\n})\n");

    ASSERT_EQ(client.explain_requests().size(), 1u);
    const auto* req = std::get_if<UpdateTableIn>(&client.explain_requests()[0]);
    ASSERT_NE(req, nullptr);
    ASSERT_EQ(req->gsi_updates.size(), 1u);
    EXPECT_EQ(req->gsi_updates[0].action, GsiUpdate::Action::create);
    ASSERT_EQ(req->attribute_definitions.size(), 1u);
    EXPECT_EQ(req->attribute_definitions[0].name, "b");
    EXPECT_TRUE(client.update_table_requests().empty());
}

// ---- 2e-3: SET TTL (DescribeTimeToLive + UpdateTimeToLive, D-2e-7) ----------

TEST(AlterTableExec, SetTtlEnableUsesUserAttribute) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    // Default probe status is DISABLED; enabling issues UpdateTimeToLive with
    // Enabled=true and the user-supplied attribute name.
    auto out = session.run("alter table t set ttl ( expiry );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");

    ASSERT_EQ(client.update_ttl_requests().size(), 1u);
    const auto& req = client.update_ttl_requests()[0];
    EXPECT_EQ(req.table_name, "t");
    EXPECT_TRUE(req.enabled);
    EXPECT_EQ(req.attribute_name, "expiry");
}

TEST(AlterTableExec, SetTtlDisableReusesExistingAttribute) {
    ddbsh::test::FakeDDBClient client;
    // The table has TTL ENABLED on attribute "ttlattr"; disabling must reuse
    // that existing name from the probe (the original sets AttributeName from
    // the DescribeTimeToLive response on disable).
    client.set_ttl("t", ddbsh::TimeToLiveOut{"ENABLED", "ttlattr"});
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set ttl disabled;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");

    ASSERT_EQ(client.update_ttl_requests().size(), 1u);
    const auto& req = client.update_ttl_requests()[0];
    EXPECT_FALSE(req.enabled);
    EXPECT_EQ(req.attribute_name, "ttlattr");
}

TEST(AlterTableExec, SetTtlDisableWhenAlreadyDisabledShortCircuits) {
    ddbsh::test::FakeDDBClient client;
    // Default probe status is DISABLED; disabling an already-disabled table
    // short-circuits to ALTER with NO UpdateTimeToLive call (D-2e-7 #2).
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set ttl disabled;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");
    EXPECT_TRUE(client.update_ttl_requests().empty());
}

TEST(AlterTableExec, SetTtlDescribeFailurePropagates) {
    ddbsh::test::FakeDDBClient client;
    // A failed DescribeTimeToLive probe ends the command with an Error and no
    // UpdateTimeToLive call (the original's has_ttl()==false -> return -1).
    client.set_ttl_error(
        "t", Error{Errc::service_error, "describe ttl failed", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set ttl ( expiry );");
    ASSERT_FALSE(out.has_value());
    EXPECT_TRUE(client.update_ttl_requests().empty());
}

TEST(AlterTableExec, SetTtlUpdateFailurePropagates) {
    ddbsh::test::FakeDDBClient client;
    client.set_update_ttl_error(
        Error{Errc::service_error, "update ttl failed", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("alter table t set ttl ( expiry );");
    ASSERT_FALSE(out.has_value());
    // The probe succeeded, so UpdateTimeToLive was attempted before failing.
    EXPECT_EQ(client.update_ttl_requests().size(), 1u);
}

TEST(AlterTableExec, ExplainSetTtlEmitsUpdateTimeToLiveBlockNoTrailingStatus) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("explain alter table t set ttl ( expiry );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "UpdateTimeToLive({\n   \"stub\":   \"explain\"\n})\n");

    // EXPLAIN serializes only the UpdateTimeToLive request (the live probe is
    // NOT serialized) and issues no mutation.
    ASSERT_EQ(client.explain_requests().size(), 1u);
    const auto* req = std::get_if<ddbsh::UpdateTimeToLiveIn>(&client.explain_requests()[0]);
    ASSERT_NE(req, nullptr);
    EXPECT_TRUE(req->enabled);
    EXPECT_EQ(req->attribute_name, "expiry");
    EXPECT_TRUE(client.update_ttl_requests().empty());
}

TEST(AlterTableExec, ExplainSetTtlDisableWhenAlreadyDisabledEmitsAlterToken) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    // The original performs the already-disabled short-circuit BEFORE its
    // explaining() branch, so EXPLAIN of a redundant disable emits the ALTER
    // token (not an UpdateTimeToLive block) — reproduced for fidelity.
    auto out = session.run("explain alter table t set ttl disabled;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "ALTER\n");
    EXPECT_TRUE(client.explain_requests().empty());
    EXPECT_TRUE(client.update_ttl_requests().empty());
}

}  // namespace
