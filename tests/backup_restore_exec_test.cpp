// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// backup_restore_exec_test.cpp — BACKUP / DROP BACKUP / DESCRIBE BACKUP /
// RESTORE execution tests (2f).
//
// Exercises the backup/restore family offline via FakeDDBClient:
//   * parse: each verb parses to the right StatementKind + request fields,
//     incl. the RESTORE target/source slot order (N1);
//   * status verbs (BACKUP / DROP BACKUP / RESTORE both forms) render their
//     token, and a seam error propagates;
//   * DESCRIBE BACKUP renders the exact report lines (§5, `Table Id:` label —
//     D-2f-7); and
//   * EXPLAIN renders one request block per verb, with the quirky
//     `RestoreTableToPointInTimeRequest` label for RESTORE-to-PITR (R1/D-2f-5)
//     and `DeleteBackup` for DROP BACKUP (D-2f-7).

#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::StatementKind;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

// ---- BACKUP ----------------------------------------------------------------

TEST(BackupExec, CreateBackupEmitsTokenAndRequest) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("backup table t called mybackup;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "BACKUP\n");

    ASSERT_EQ(client.create_backup_requests().size(), 1u);
    EXPECT_EQ(client.create_backup_requests()[0].table_name, "t");
    EXPECT_EQ(client.create_backup_requests()[0].backup_name, "mybackup");
}

TEST(BackupExec, CreateBackupParsesToKind) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = session.parse("backup table t called mybackup;");
    ASSERT_TRUE(stmt.has_value());
    EXPECT_EQ(stmt->kind(), StatementKind::create_backup);
    EXPECT_FALSE(stmt->explain());
}

TEST(BackupExec, CreateBackupExplain) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto out = session.run("explain backup table t called mybackup;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // Pretty format: tab -> 3 spaces; one CreateBackup block, no status token.
    EXPECT_EQ(render(*out), "CreateBackup({\n   \"stub\":   \"explain\"\n})\n");
    EXPECT_TRUE(client.create_backup_requests().empty());  // EXPLAIN issues no call
}

TEST(BackupExec, CreateBackupFailurePropagates) {
    ddbsh::test::FakeDDBClient client;
    client.set_create_backup_error(Error{Errc::service_error, "boom", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);
    auto out = session.run("backup table t called b;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "boom");
}

// ---- DROP BACKUP -----------------------------------------------------------

TEST(BackupExec, DropBackupEmitsTokenAndRequest) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("drop backup \"arn:aws:dynamodb:us-east-1:1:table/t/backup/01\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "DROP BACKUP\n");

    ASSERT_EQ(client.delete_backup_requests().size(), 1u);
    EXPECT_EQ(client.delete_backup_requests()[0].backup_arn,
              "arn:aws:dynamodb:us-east-1:1:table/t/backup/01");
}

TEST(BackupExec, DropBackupExplainEmitsDeleteBackup) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto out = session.run("explain drop backup \"arn:x\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // D-2f-7: the EXPLAIN label is DeleteBackup (not DescribeBackup — arch §12
    // #1 is stale).
    EXPECT_EQ(render(*out), "DeleteBackup({\n   \"stub\":   \"explain\"\n})\n");
}

TEST(BackupExec, DropBackupParsesToKind) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto stmt = session.parse("drop backup \"arn:x\";");
    ASSERT_TRUE(stmt.has_value());
    EXPECT_EQ(stmt->kind(), StatementKind::drop_backup);
}

// ---- DESCRIBE BACKUP -------------------------------------------------------

ddbsh::DescribeBackupOut canned_backup() {
    ddbsh::DescribeBackupOut b;
    b.source_table_name = "t";
    b.source_table_id = "id-123";
    b.source_table_arn = "arn:aws:dynamodb:us-east-1:1:table/t";
    b.table_size_bytes = 1024;
    b.item_count = 7;
    b.key_schema = {{"HASH", "pk"}, {"RANGE", "sk"}};
    b.table_creation_datetime = "2023-01-01T00:00:00Z";
    b.billing_mode = "PROVISIONED";
    b.provisioned = std::make_pair<long long, long long>(5, 3);
    b.backup_name = "mybackup";
    b.backup_arn = "arn:aws:dynamodb:us-east-1:1:table/t/backup/01";
    b.backup_size_bytes = 2048;
    b.backup_status = "AVAILABLE";
    b.backup_type = "USER";
    b.backup_creation_datetime = "2023-02-01T00:00:00Z";
    b.backup_expiry_datetime = "2023-03-01T00:00:00Z";
    return b;
}

TEST(BackupExec, DescribeBackupReportLines) {
    ddbsh::test::FakeDDBClient client;
    client.set_describe_backup(canned_backup());
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe backup \"arn:aws:dynamodb:us-east-1:1:table/t/backup/01\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    const std::string expected =
        "Table Name: t\n"
        "Table Id: id-123\n"
        "Table Arn: arn:aws:dynamodb:us-east-1:1:table/t\n"
        "Table Size (bytes): 1024\n"
        "Item Count: 7\n"
        "Table Key: ( HASH pk, RANGE sk )\n"
        "Table Creation Date/Time: 2023-01-01T00:00:00Z\n"
        "Billing Mode: PROVISIONED\n"
        "Provisioned Throughput: 5 RCU, 3 WCU\n"
        "Backup Name: mybackup\n"
        "Backup Arn: arn:aws:dynamodb:us-east-1:1:table/t/backup/01\n"
        "Backup Size (bytes): 2048\n"
        "Backup Status: AVAILABLE\n"
        "Backup Type: USER\n"
        "Backup Creation Date/Time: 2023-02-01T00:00:00Z\n"
        "Backup Expiry Date/Time: 2023-03-01T00:00:00Z\n";
    EXPECT_EQ(render(*out), expected);
}

TEST(BackupExec, DescribeBackupOmitsOptionalLinesWhenUnset) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::DescribeBackupOut b = canned_backup();
    b.billing_mode = std::nullopt;
    b.provisioned = std::nullopt;
    client.set_describe_backup(b);
    ddbsh::Session session = make_session(client);

    auto out = session.run("describe backup \"arn:x\";");
    ASSERT_TRUE(out.has_value());
    const std::string rendered = render(*out);
    EXPECT_EQ(rendered.find("Billing Mode:"), std::string::npos);
    EXPECT_EQ(rendered.find("Provisioned Throughput:"), std::string::npos);
}

TEST(BackupExec, DescribeBackupExplain) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto out = session.run("explain describe backup \"arn:x\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "DescribeBackup({\n   \"stub\":   \"explain\"\n})\n");
}

TEST(BackupExec, DescribeBackupFailurePropagates) {
    ddbsh::test::FakeDDBClient client;  // no canned backup => error
    ddbsh::Session session = make_session(client);
    auto out = session.run("describe backup \"arn:x\";");
    ASSERT_FALSE(out.has_value());
}

// ---- RESTORE ---------------------------------------------------------------

TEST(RestoreExec, FromBackupSlotOrderAndToken) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("restore table newt from backup \"arn:src\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "RESTORE\n");

    ASSERT_EQ(client.restore_from_backup_requests().size(), 1u);
    // N1: `newt` is the TARGET (after K_TABLE); the ARN is the SOURCE (K_FROM).
    EXPECT_EQ(client.restore_from_backup_requests()[0].target_table_name, "newt");
    EXPECT_EQ(client.restore_from_backup_requests()[0].backup_arn, "arn:src");
}

TEST(RestoreExec, FromBackupExplain) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto out = session.run("explain restore table newt from backup \"arn:src\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "RestoreTableFromBackup({\n   \"stub\":   \"explain\"\n})\n");
}

TEST(RestoreExec, ToPitrSlotOrderAndToken) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto out = session.run("restore table newt from srct to pitr \"2023-01-01T00:00:00Z\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "RESTORE\n");

    ASSERT_EQ(client.restore_to_pitr_requests().size(), 1u);
    const auto& req = client.restore_to_pitr_requests()[0];
    EXPECT_EQ(req.target_table_name, "newt");                 // N1: target after K_TABLE
    EXPECT_EQ(req.source_table_name, "srct");                 // N1: source after K_FROM
    EXPECT_EQ(req.restore_datetime, "2023-01-01T00:00:00Z");  // N2: unquoted
}

TEST(RestoreExec, ToPitrExplainHasRequestSuffixedLabel) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto out =
        session.run("explain restore table newt from srct to pitr \"2023-01-01T00:00:00Z\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // R1/D-2f-5: the label carries the trailing `Request`.
    EXPECT_EQ(render(*out), "RestoreTableToPointInTimeRequest({\n   \"stub\":   \"explain\"\n})\n");
}

TEST(RestoreExec, ParsesToRestoreKind) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto a = session.parse("restore table newt from backup \"arn\";");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->kind(), StatementKind::restore);
    auto b = session.parse("restore table newt from srct to pitr \"2023-01-01T00:00:00Z\";");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->kind(), StatementKind::restore);
}

TEST(RestoreExec, FailurePropagates) {
    ddbsh::test::FakeDDBClient client;
    client.set_restore_from_backup_error(
        Error{Errc::service_error, "nope", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);
    auto out = session.run("restore table newt from backup \"arn\";");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "nope");
}

}  // namespace
