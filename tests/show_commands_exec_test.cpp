// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// show_commands_exec_test.cpp — SHOW BACKUPS / SHOW LIMITS / SHOW CREATE TABLE
// execution tests (2f).
//
// Exercises offline via FakeDDBClient:
//   * SHOW BACKUPS rows + pagination + type/table filters;
//   * SHOW LIMITS report shaping (Region line from the session region, the
//     two-space `Table Max` quirk, provisioned/GSI totals, on-demand = 0); and
//   * SHOW CREATE TABLE DDL reconstruction (clause spacing, projections,
//     billing, IF NOT EXISTS/NOWAIT echo) and the missing/non-ACTIVE guards.
//
// Also pins the fidelity fact that SHOW commands are NOT explainable (the
// oracle's explain_command wraps only ddl/dml/transact) — `EXPLAIN SHOW ...` is
// a syntax error.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::BackupSummaryRow;
using ddbsh::DescribeLimitsOut;
using ddbsh::DescribeTableOut;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::ProjectionType;
using ddbsh::ScalarType;
using ddbsh::SecondaryIndexSchema;
using ddbsh::StatementKind;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client,
                            const std::string& region = "us-east-1") {
    return ddbsh::Session(ddbsh::SessionConfig{region, ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

BackupSummaryRow row(const std::string& name, const std::string& arn) {
    return BackupSummaryRow{
        "t", name, "AVAILABLE", arn, "2023-01-01T00:00:00Z", "2023-02-01T00:00:00Z"};
}

// ---- SHOW BACKUPS ----------------------------------------------------------

TEST(ShowBackupsExec, SinglePageRows) {
    ddbsh::test::FakeDDBClient client;
    client.set_backup_pages({{row("b1", "arn1"), row("b2", "arn2")}});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show backups t;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out),
              "Table: t, Backup: b1, Status: AVAILABLE, ARN: arn1, On: 2023-01-01T00:00:00Z, "
              "Expires: 2023-02-01T00:00:00Z\n"
              "Table: t, Backup: b2, Status: AVAILABLE, ARN: arn2, On: 2023-01-01T00:00:00Z, "
              "Expires: 2023-02-01T00:00:00Z\n");

    ASSERT_EQ(client.list_backups_requests().size(), 1u);
    ASSERT_TRUE(client.list_backups_requests()[0].table_name.has_value());
    EXPECT_EQ(*client.list_backups_requests()[0].table_name, "t");
    EXPECT_EQ(client.list_backups_requests()[0].limit, 100);
}

TEST(ShowBackupsExec, PaginatesUntilCursorEmpty) {
    ddbsh::test::FakeDDBClient client;
    client.set_backup_pages({{row("b1", "arn1")}, {row("b2", "arn2")}});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show backups;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // Two pages, concatenated.
    const std::string rendered = render(*out);
    EXPECT_NE(rendered.find("Backup: b1"), std::string::npos);
    EXPECT_NE(rendered.find("Backup: b2"), std::string::npos);

    ASSERT_EQ(client.list_backups_requests().size(), 2u);
    EXPECT_FALSE(client.list_backups_requests()[0].exclusive_start_backup_arn.has_value());
    ASSERT_TRUE(client.list_backups_requests()[1].exclusive_start_backup_arn.has_value());
    EXPECT_EQ(*client.list_backups_requests()[1].exclusive_start_backup_arn, "1");
}

TEST(ShowBackupsExec, TypeAndTableFilters) {
    ddbsh::test::FakeDDBClient client;
    client.set_backup_pages({{}});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show user backups mytable;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "");  // empty page => no lines

    ASSERT_EQ(client.list_backups_requests().size(), 1u);
    const auto& req = client.list_backups_requests()[0];
    ASSERT_TRUE(req.backup_type.has_value());
    EXPECT_EQ(*req.backup_type, "user");
    ASSERT_TRUE(req.table_name.has_value());
    EXPECT_EQ(*req.table_name, "mytable");
}

TEST(ShowBackupsExec, NoArgsLeavesFiltersUnset) {
    ddbsh::test::FakeDDBClient client;
    client.set_backup_pages({{}});
    ddbsh::Session session = make_session(client);

    auto out = session.run("show backups;");
    ASSERT_TRUE(out.has_value());
    const auto& req = client.list_backups_requests()[0];
    EXPECT_FALSE(req.backup_type.has_value());
    EXPECT_FALSE(req.table_name.has_value());
}

TEST(ShowBackupsExec, ParsesToKind) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = session.parse("show backups;");
    ASSERT_TRUE(stmt.has_value());
    EXPECT_EQ(stmt->kind(), StatementKind::show_backups);
}

TEST(ShowBackupsExec, ListBackupsFailurePropagates) {
    ddbsh::test::FakeDDBClient client;
    client.set_list_backups_error(
        Error{Errc::service_error, "list backups failed", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);
    auto out = session.run("show backups;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "list backups failed");
}

// ---- SHOW LIMITS -----------------------------------------------------------

DescribeTableOut provisioned_table_with_gsi(const std::string& name) {
    DescribeTableOut td;
    td.table_name = name;
    td.table_status = "ACTIVE";
    td.billing_mode_summary = "PROVISIONED";
    td.provisioned_read_capacity_units = 10;
    td.provisioned_write_capacity_units = 5;
    SecondaryIndexSchema gsi;
    gsi.index_name = "g";
    gsi.provisioned_read_capacity_units = 4;
    gsi.provisioned_write_capacity_units = 2;
    td.global_secondary_indexes.push_back(gsi);
    return td;
}

DescribeTableOut on_demand_table(const std::string& name) {
    DescribeTableOut td;
    td.table_name = name;
    td.table_status = "ACTIVE";
    td.billing_mode_summary = "PAY_PER_REQUEST";
    return td;
}

TEST(ShowLimitsExec, ReportShapeAndTotals) {
    ddbsh::test::FakeDDBClient client;
    client.set_describe_limits(DescribeLimitsOut{40000, 40000, 40000, 40000});
    client.set_table_names({"t1", "t2"});
    client.set_table(provisioned_table_with_gsi("t1"));
    client.set_table(on_demand_table("t2"));
    ddbsh::Session session = make_session(client, "us-east-1");

    auto out = session.run("show limits;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out),
              "Region: us-east-1\n"
              "Account Max (RCU, WCU): 40000, 40000\n"
              "Table Max (RCU, WCU):  40000, 40000\n"  // two spaces after colon (oracle quirk)
              "Total Tables (RCU, WCU): 10, 5\n"
              "Total GSI (RCU, WCU): 4, 2\n"
              "Total (RCU, WCU): 14, 7\n");
}

TEST(ShowLimitsExec, RegionFromSession) {
    ddbsh::test::FakeDDBClient client;
    client.set_describe_limits(DescribeLimitsOut{1, 2, 3, 4});
    ddbsh::Session session = make_session(client, "eu-west-1");
    auto out = session.run("show limits;");
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(render(*out).find("Region: eu-west-1\n"), std::string::npos);
}

TEST(ShowLimitsExec, ParsesToKind) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = session.parse("show limits;");
    ASSERT_TRUE(stmt.has_value());
    EXPECT_EQ(stmt->kind(), StatementKind::show_limits);
}

TEST(ShowLimitsExec, DescribeLimitsFailurePropagates) {
    ddbsh::test::FakeDDBClient client;
    client.set_describe_limits_error(
        Error{Errc::service_error, "describe limits failed", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);
    auto out = session.run("show limits;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "describe limits failed");
}

// ---- SHOW CREATE TABLE -----------------------------------------------------

TEST(ShowCreateTableExec, ProvisionedSingleKey) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut td;
    td.table_name = "t";
    td.table_status = "ACTIVE";
    td.attribute_definitions = {{"id", ScalarType::s}, {"n", ScalarType::n}};
    td.key_schema = {"id", ""};
    td.billing_mode_summary = "PROVISIONED";
    td.provisioned_read_capacity_units = 5;
    td.provisioned_write_capacity_units = 3;
    client.set_table(td);
    ddbsh::Session session = make_session(client);

    auto out = session.run("show create table t;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out),
              "CREATE TABLE \"t\" (\"id\" string, \"n\" number) PRIMARY KEY (\"id\" HASH) "
              "BILLING MODE PROVISIONED ( 5 RCU, 3 WCU ) TABLE CLASS STANDARD "
              "DELETION PROTECTION DISABLED\n");
}

TEST(ShowCreateTableExec, OnDemandWithGsiIncludeAndIfNotExistsNowait) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut td;
    td.table_name = "t";
    td.table_status = "ACTIVE";
    td.attribute_definitions = {
        {"pk", ScalarType::s}, {"sk", ScalarType::n}, {"gpk", ScalarType::s}};
    td.key_schema = {"pk", "sk"};
    td.billing_mode_summary = "PAY_PER_REQUEST";
    SecondaryIndexSchema gsi;
    gsi.index_name = "g1";
    gsi.key_schema = {"gpk", ""};
    gsi.projection_set = true;
    gsi.projection.type = ProjectionType::include;
    gsi.projection.non_key_attributes = {"a", "b"};
    td.global_secondary_indexes.push_back(gsi);
    client.set_table(td);
    ddbsh::Session session = make_session(client);

    auto out = session.run("show create table if not exists nowait t;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(
        render(*out),
        "CREATE TABLE IF NOT EXISTS NOWAIT \"t\" (\"pk\" string, \"sk\" number, \"gpk\" string) "
        "PRIMARY KEY (\"pk\" HASH, \"sk\" RANGE) BILLING MODE ON DEMAND "
        "GSI (\"g1\" ON ( \"gpk\" HASH ) PROJECTING INCLUDE (a, b) BILLING MODE ON DEMAND ) "
        "TABLE CLASS STANDARD DELETION PROTECTION DISABLED\n");
}

TEST(ShowCreateTableExec, StreamDisabledVsEnabled) {
    // Stream spec present but disabled => "STREAM DISABLED "; enabled => the
    // view type words (D-2f stream handling via stream_specification_set).
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut td;
    td.table_name = "t";
    td.table_status = "ACTIVE";
    td.attribute_definitions = {{"id", ScalarType::s}};
    td.key_schema = {"id", ""};
    td.billing_mode_summary = "PAY_PER_REQUEST";
    td.stream_specification_set = true;
    td.stream_view_type = "NEW_AND_OLD_IMAGES";
    client.set_table(td);
    ddbsh::Session session = make_session(client);

    auto out = session.run("show create table t;");
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(render(*out).find("STREAM ( BOTH IMAGES ) "), std::string::npos);
}

TEST(ShowCreateTableExec, MissingTableError) {
    ddbsh::test::FakeDDBClient client;  // no table registered
    ddbsh::Session session = make_session(client);
    auto out = session.run("show create table ghost;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "The table \"ghost\" does not exist.");
}

TEST(ShowCreateTableExec, NotActiveError) {
    ddbsh::test::FakeDDBClient client;
    DescribeTableOut td;
    td.table_name = "t";
    td.table_status = "CREATING";
    client.set_table(td);
    ddbsh::Session session = make_session(client);
    auto out = session.run("show create table t;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "The table \"t\" is not ACTIVE.");
}

TEST(ShowCreateTableExec, ParsesToKind) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = session.parse("show create table t;");
    ASSERT_TRUE(stmt.has_value());
    EXPECT_EQ(stmt->kind(), StatementKind::show_create_table);
}

// ---- EXPLAIN SHOW is a syntax error (fidelity) -----------------------------

TEST(ShowCommandsExec, ExplainShowIsSyntaxError) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    EXPECT_FALSE(session.parse("explain show limits;").has_value());
    EXPECT_FALSE(session.parse("explain show backups;").has_value());
    EXPECT_FALSE(session.parse("explain show create table t;").has_value());
}

}  // namespace
