// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_client_control_test.cpp — WireClient control-plane / transaction /
// waiter coverage over a CannedTransport, plus the Session-over-WireClient
// round-trip (P6c-b; the design §6.3, §6.4).
//
// Extends wire_client_test.cpp (the data-plane round-trips) with the P6c-b
// surface: the ListTables internal paging loop, the five composed-error
// wrappers (§4.3), the two table waiters (scripted poll sequences, with the
// poll delay overridden to a no-op so the loop runs instantly), EXPLAIN for the
// previously-stubbed alternatives, and — now that describe_table lands — a real
// ddbsh::Session driving the full engine over the wire client.

#include <chrono>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/testing/canned_transport.hpp"
#include "ddbsh/value.hpp"
#include "ddbsh/wire/codec.hpp"
#include "ddbsh/wire/wire_client.hpp"

namespace {

using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::ExplainRequest;
using ddbsh::Value;
using ddbsh::testing::CannedTransport;
using ddbsh::wire::WireClient;

// A WireClient whose waiter backoff sleep is a no-op, so the scripted poll loops
// run instantly in the test (no real sleep). backoff_sleep is the single
// platform-coupled seam (D-P6c-6; shared with the HT-1 retry path); overriding
// it exercises the waiter logic without wall-clock cost.
class NoSleepWireClient : public WireClient {
public:
    using WireClient::WireClient;

protected:
    void backoff_sleep(std::chrono::milliseconds) override {}
};

// ---- ListTables internal paging --------------------------------------------

TEST(WireClientControl, ListTablesPagesInternally) {
    CannedTransport transport;
    transport.push_response(R"({"TableNames":["a","b"],"LastEvaluatedTableName":"b"})");
    transport.push_response(R"({"TableNames":["c"]})");
    WireClient client(transport);

    auto out = client.list_tables(ddbsh::ListTablesIn{});
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->table_names, (std::vector<std::string>{"a", "b", "c"}));

    ASSERT_EQ(transport.calls().size(), 2u);
    EXPECT_EQ(transport.calls()[0].operation, "ListTables");
    EXPECT_EQ(transport.calls()[0].body, "{\n}\n");  // first page: empty body
    // Second page threads the cursor as ExclusiveStartTableName.
    EXPECT_EQ(transport.calls()[1].body,
              ddbsh::wire::serialize_list_tables(std::optional<std::string>("b")).body);
    EXPECT_TRUE(transport.drained());
}

// ---- ListBackups: one page per call (the executor drives the cursor) -------

TEST(WireClientControl, ListBackupsReturnsOnePage) {
    CannedTransport transport;
    transport.push_response(R"({"BackupSummaries":[
        {"TableName":"t","BackupName":"b1","BackupStatus":"AVAILABLE","BackupArn":"arn:b1",
         "BackupCreationDateTime":1700000000,"BackupExpiryDateTime":1700000000}],
        "LastEvaluatedBackupArn":"arn:cursor"})");
    WireClient client(transport);

    ddbsh::ListBackupsIn in;
    auto out = client.list_backups(in);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->summaries.size(), 1u);
    ASSERT_TRUE(out->last_evaluated_backup_arn.has_value());
    EXPECT_EQ(*out->last_evaluated_backup_arn, "arn:cursor");
    EXPECT_EQ(transport.calls().size(), 1u);  // one page; no internal loop
    EXPECT_EQ(transport.calls()[0].operation, "ListBackups");
}

// ---- composed error messages (§4.3) ----------------------------------------

TEST(WireClientControl, TransactWriteComposesFailureMessage) {
    CannedTransport transport;
    transport.push_response(
        R"({"__type":"x#TransactionCanceledException","message":"Transaction cancelled, please refer to ..."})");
    WireClient client(transport);

    ddbsh::TransactWriteItemsIn in;
    ddbsh::TransactPut put;
    put.table_name = "t";
    put.item = {{"id", Value::string_value("k1")}};
    in.items.push_back(put);
    auto out = client.transact_write_items(in);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    // Empty reqid segment on the wire path (D-P6c-5).
    EXPECT_EQ(out.error().message,
              "Transaction failed. TransactionCanceledException. . "
              "Transaction cancelled, please refer to ...");
}

TEST(WireClientControl, TransactGetComposesFailureMessage) {
    CannedTransport transport;
    transport.push_response(R"({"__type":"x#TransactionCanceledException","message":"cancelled"})");
    WireClient client(transport);

    ddbsh::TransactGetItemsIn in;
    ddbsh::TransactGet get;
    get.table_name = "t";
    get.key = {{"id", Value::string_value("k1")}};
    in.items.push_back(get);
    auto out = client.transact_get_items(in);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "Transaction failed. TransactionCanceledException. . cancelled");
}

TEST(WireClientControl, UpdateTableComposesFailureMessage) {
    CannedTransport transport;
    transport.push_response(R"({"__type":"x#ValidationException","message":"bad update"})");
    WireClient client(transport);

    ddbsh::UpdateTableIn in;
    in.table_name = "t";
    in.deletion_protection = true;
    auto out = client.update_table(in);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "Update Table failed. ValidationException. . bad update");
}

TEST(WireClientControl, UpdateTtlComposesFailureMessageWithNewlineTab) {
    CannedTransport transport;
    transport.push_response(R"({"__type":"x#ValidationException","message":"bad ttl"})");
    WireClient client(transport);

    ddbsh::UpdateTimeToLiveIn in;
    in.table_name = "t";
    in.enabled = true;
    in.attribute_name = "ttl";
    auto out = client.update_time_to_live(in);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "Error updating TTL. ValidationException. .\n\tbad ttl");
}

TEST(WireClientControl, UpdatePitrComposesFailureMessageWithNewlineTab) {
    CannedTransport transport;
    transport.push_response(R"({"__type":"x#ValidationException","message":"bad pitr"})");
    WireClient client(transport);

    ddbsh::UpdateContinuousBackupsIn in;
    in.table_name = "t";
    in.enabled = true;
    auto out = client.update_continuous_backups(in);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "Setting PITR failed. ValidationException. .\n\tbad pitr");
}

// ---- RestoreToPitr validates the timestamp before the transport -----------

TEST(WireClientControl, RestoreToPitrRejectsBadTimestampBeforeSend) {
    CannedTransport transport;
    WireClient client(transport);

    ddbsh::RestoreToPitrIn in;
    in.source_table_name = "s";
    in.target_table_name = "t";
    in.restore_datetime = "nonsense";
    auto out = client.restore_table_to_point_in_time(in);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::invalid_argument);
    EXPECT_EQ(out.error().message, "Invalid restore timestamp: nonsense");
    EXPECT_TRUE(transport.calls().empty());  // never reached the transport
}

TEST(WireClientControl, RestoreToPitrValidTimestampSends) {
    CannedTransport transport;
    transport.push_response(R"({})");
    WireClient client(transport);

    ddbsh::RestoreToPitrIn in;
    in.source_table_name = "s";
    in.target_table_name = "t";
    in.restore_datetime = "2023-11-14T22:13:20Z";
    auto out = client.restore_table_to_point_in_time(in);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(transport.calls().size(), 1u);
    EXPECT_EQ(transport.calls()[0].operation, "RestoreTableToPointInTime");
}

// ---- table waiters ---------------------------------------------------------

TEST(WireClientControl, WaitForActivePollsThenSucceeds) {
    CannedTransport transport;
    transport.push_response(R"({"Table":{"TableName":"t","TableStatus":"CREATING"}})");
    transport.push_response(R"({"Table":{"TableName":"t","TableStatus":"ACTIVE"}})");
    NoSleepWireClient client(transport);

    auto out = client.wait_for_table_active("t");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(transport.calls().size(), 2u);
    EXPECT_EQ(transport.calls()[0].operation, "DescribeTable");
}

TEST(WireClientControl, WaitForActiveNotFoundThenActive) {
    CannedTransport transport;
    transport.push_response(R"({"__type":"x#ResourceNotFoundException","message":"not yet"})");
    transport.push_response(R"({"Table":{"TableName":"t","TableStatus":"ACTIVE"}})");
    NoSleepWireClient client(transport);

    auto out = client.wait_for_table_active("t");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(transport.calls().size(), 2u);
}

TEST(WireClientControl, WaitForActiveTerminalStatusErrors) {
    CannedTransport transport;
    transport.push_response(R"({"Table":{"TableName":"t","TableStatus":"ARCHIVED"}})");
    NoSleepWireClient client(transport);

    auto out = client.wait_for_table_active("t");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "Table t is in state ARCHIVED and will not become ACTIVE.");
}

TEST(WireClientControl, WaitForActiveNonRetryableErrorIsTerminal) {
    CannedTransport transport;
    transport.push_response(R"({"__type":"x#AccessDeniedException","message":"denied"})");
    NoSleepWireClient client(transport);

    auto out = client.wait_for_table_active("t");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "denied");
}

TEST(WireClientControl, WaitForGonePollsThenSucceeds) {
    CannedTransport transport;
    transport.push_response(R"({"Table":{"TableName":"t","TableStatus":"DELETING"}})");
    transport.push_response(R"({"__type":"x#ResourceNotFoundException","message":"gone"})");
    NoSleepWireClient client(transport);

    auto out = client.wait_for_table_gone("t");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(transport.calls().size(), 2u);
}

TEST(WireClientControl, WaitForGoneNonRetryableErrorIsTerminal) {
    CannedTransport transport;
    transport.push_response(R"({"__type":"x#AccessDeniedException","message":"denied"})");
    NoSleepWireClient client(transport);

    auto out = client.wait_for_table_gone("t");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().message, "denied");
}

// ---- EXPLAIN completeness: every alternative serializes, none errors -------

TEST(WireClientControl, ExplainControlAlternativesReturnSerializedBody) {
    CannedTransport transport;
    WireClient client(transport);

    auto check = [&](const ExplainRequest& req, const std::string& expected) {
        auto body = client.explain_request(req);
        ASSERT_TRUE(body.has_value());
        EXPECT_EQ(*body, expected);
    };

    ddbsh::DeleteTableIn dt;
    dt.table_name = "t";
    check(dt, ddbsh::wire::serialize(dt).body);

    ddbsh::DescribeTimeToLiveIn ttl;
    ttl.table_name = "t";
    check(ttl, ddbsh::wire::serialize(ttl).body);

    ddbsh::DescribeContinuousBackupsIn cb;
    cb.table_name = "t";
    check(cb, ddbsh::wire::serialize(cb).body);

    ddbsh::DescribeTableReplicaAutoScalingIn as;
    as.table_name = "t";
    check(as, ddbsh::wire::serialize(as).body);

    ddbsh::CreateBackupIn cbk;
    cbk.table_name = "t";
    cbk.backup_name = "bk";
    check(cbk, ddbsh::wire::serialize(cbk).body);

    ddbsh::DescribeBackupIn dbk;
    dbk.backup_arn = "arn:bk";
    check(dbk, ddbsh::wire::serialize(dbk).body);

    EXPECT_TRUE(transport.calls().empty());  // EXPLAIN never hits the transport
}

// ---- Session over WireClient: the full engine, SDK-free, offline -----------

ddbsh::Session make_session(ddbsh::IDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ddbsh::ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

TEST(WireClientSession, SelectGetItemRoundTrip) {
    CannedTransport transport;
    // The planner resolves the key schema via DescribeTable, then GetItem.
    transport.push_response(
        R"({"Table":{"TableName":"t","KeySchema":[{"AttributeName":"a","KeyType":"HASH"}],)"
        R"("AttributeDefinitions":[{"AttributeName":"a","AttributeType":"N"}],"TableStatus":"ACTIVE"}})");
    transport.push_response(R"({"Item":{"a":{"N":"2"},"c":{"N":"9"}}})");
    NoSleepWireClient client(transport);
    auto session = make_session(client);

    auto out = session.run("select * from t where a = 2;");
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().message);
    EXPECT_EQ(render(*out), "{a: 2, c: 9}\n");

    ASSERT_EQ(transport.calls().size(), 2u);
    EXPECT_EQ(transport.calls()[0].operation, "DescribeTable");
    EXPECT_EQ(transport.calls()[1].operation, "GetItem");
}

TEST(WireClientSession, CreateTableNowaitRoundTrip) {
    CannedTransport transport;
    transport.push_response(R"({"TableDescription":{"TableStatus":"CREATING"}})");
    NoSleepWireClient client(transport);
    auto session = make_session(client);

    auto out = session.run("create table nowait t ( a number ) primary key ( a hash );");
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().message);
    EXPECT_EQ(render(*out), "CREATE\n");

    ASSERT_EQ(transport.calls().size(), 1u);  // CreateTable only (NOWAIT => no waiter)
    EXPECT_EQ(transport.calls()[0].operation, "CreateTable");
}

TEST(WireClientSession, ExplainSelectResolvesSchemaButDoesNotExecute) {
    CannedTransport transport;
    transport.push_response(
        R"({"Table":{"TableName":"t","KeySchema":[{"AttributeName":"a","KeyType":"HASH"}],)"
        R"("AttributeDefinitions":[{"AttributeName":"a","AttributeType":"N"}],"TableStatus":"ACTIVE"}})");
    NoSleepWireClient client(transport);
    auto session = make_session(client);

    auto out = session.run("explain select * from t where a = 3;");
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().message);
    const std::string rendered = render(*out);
    EXPECT_EQ(rendered.rfind("GetItem(", 0), 0u);  // full key => GetItem, serialized body

    // Only the schema DescribeTable hit the transport; EXPLAIN did not execute.
    ASSERT_EQ(transport.calls().size(), 1u);
    EXPECT_EQ(transport.calls()[0].operation, "DescribeTable");
}

}  // namespace
