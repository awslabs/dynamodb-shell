// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_client_test.cpp — WireClient end-to-end over a CannedTransport (P6b).
//
// Proves the serialize -> send -> parse pipeline for each data-plane op (the
// real "host round-trip" of §14 P6b, offline), the ccf and transport-failure
// error paths, the SDK-free data-plane EXPLAIN, and the labelled P6c stubs. The
// CannedTransport both scripts the response and records the exact (operation,
// body) the client produced, so the request bytes are asserted too.

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/testing/canned_transport.hpp"
#include "ddbsh/value.hpp"
#include "ddbsh/wire/codec.hpp"
#include "ddbsh/wire/wire_client.hpp"

namespace {

using ddbsh::DeleteItemIn;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::ExplainRequest;
using ddbsh::GetItemIn;
using ddbsh::PutItemIn;
using ddbsh::QueryIn;
using ddbsh::ScanIn;
using ddbsh::UpdateItemIn;
using ddbsh::Value;
using ddbsh::testing::CannedTransport;
using ddbsh::wire::WireClient;

GetItemIn sample_get() {
    GetItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    return in;
}

// ---- data-plane happy paths ------------------------------------------------

TEST(WireClient, GetItemRoundTrip) {
    CannedTransport transport;
    transport.push_response(R"({"Item":{"id":{"S":"k1"},"v":{"N":"7"}}})");
    WireClient client(transport);

    const GetItemIn in = sample_get();
    auto out = client.get_item(in);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->item.has_value());
    EXPECT_EQ(out->item->attributes.size(), 2u);

    ASSERT_EQ(transport.calls().size(), 1u);
    EXPECT_EQ(transport.calls()[0].operation, "GetItem");
    EXPECT_EQ(transport.calls()[0].body, ddbsh::wire::serialize(in).body);
    EXPECT_TRUE(transport.drained());
}

TEST(WireClient, QueryRoundTrip) {
    CannedTransport transport;
    transport.push_response(R"({"Items":[{"id":{"S":"a"}},{"id":{"S":"b"}}]})");
    WireClient client(transport);

    QueryIn in;
    in.table_name = "t";
    in.key_condition_expression = "#aaaa1 = :vaaa1";
    in.expression_attribute_names = {{"#aaaa1", "id"}};
    in.expression_attribute_values = {{":vaaa1", Value::string_value("a")}};
    auto out = client.query(in);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->items.size(), 2u);
    EXPECT_EQ(transport.calls()[0].operation, "Query");
}

TEST(WireClient, ScanRoundTrip) {
    CannedTransport transport;
    transport.push_response(R"({"Items":[{"id":{"S":"a"}}]})");
    WireClient client(transport);

    ScanIn in;
    in.table_name = "t";
    auto out = client.scan(in);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->items.size(), 1u);
    EXPECT_EQ(transport.calls()[0].operation, "Scan");
}

TEST(WireClient, PutItemRoundTrip) {
    CannedTransport transport;
    transport.push_response(R"({})");  // PutItem with no ReturnValues -> empty body
    WireClient client(transport);

    PutItemIn in;
    in.table_name = "t";
    in.item = {{"id", Value::string_value("k1")}};
    auto out = client.put_item(in);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->attributes.has_value());
    EXPECT_EQ(transport.calls()[0].operation, "PutItem");
    EXPECT_EQ(transport.calls()[0].body, ddbsh::wire::serialize(in).body);
}

TEST(WireClient, UpdateItemRoundTrip) {
    CannedTransport transport;
    transport.push_response(R"({"Attributes":{"v":{"N":"2"}}})");
    WireClient client(transport);

    UpdateItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    in.update_expression = "SET #aaaa1 = :vaaa1";
    in.expression_attribute_names = {{"#aaaa1", "v"}};
    in.expression_attribute_values = {{":vaaa1", Value::number_value("2")}};
    auto out = client.update_item(in);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->attributes.has_value());
    EXPECT_EQ(transport.calls()[0].operation, "UpdateItem");
}

TEST(WireClient, DeleteItemRoundTrip) {
    CannedTransport transport;
    transport.push_response(R"({})");
    WireClient client(transport);

    DeleteItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    auto out = client.delete_item(in);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(transport.calls()[0].operation, "DeleteItem");
}

// ---- error paths -----------------------------------------------------------

TEST(WireClient, ConditionalCheckFailedIsMapped) {
    CannedTransport transport;
    transport.push_response(
        R"({"__type":"x#ConditionalCheckFailedException","message":"The conditional request failed"})");
    WireClient client(transport);

    UpdateItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    in.update_expression = "SET #aaaa1 = :vaaa1";
    in.expression_attribute_names = {{"#aaaa1", "v"}};
    in.expression_attribute_values = {{":vaaa1", Value::number_value("2")}};
    auto out = client.update_item(in);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::conditional_check_failed);
    EXPECT_EQ(out.error().message, "The conditional request failed");
}

TEST(WireClient, TransportFailureSurfacedUnchanged) {
    CannedTransport transport;
    transport.push_error(
        Error{Errc::service_error, "backend unreachable", std::nullopt, std::nullopt});
    WireClient client(transport);

    auto out = client.get_item(sample_get());
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "backend unreachable");
}

// ---- EXPLAIN ---------------------------------------------------------------

TEST(WireClient, ExplainDataPlaneReturnsSerializedBody) {
    CannedTransport transport;
    WireClient client(transport);

    const GetItemIn in = sample_get();
    ExplainRequest req = in;
    auto body = client.explain_request(req);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(*body, ddbsh::wire::serialize(in).body);
    EXPECT_TRUE(transport.calls().empty());  // EXPLAIN makes no transport call
}

TEST(WireClient, ExplainControlPlaneReturnsSerializedBody) {
    CannedTransport transport;
    WireClient client(transport);

    // P6c: every ExplainRequest alternative now serializes SDK-free. A
    // control-plane op (DescribeTable) returns its serialized body and makes no
    // transport call.
    ddbsh::DescribeTableIn dt;
    dt.table_name = "t";
    ExplainRequest req = dt;
    auto body = client.explain_request(req);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(*body, ddbsh::wire::serialize(dt).body);
    EXPECT_TRUE(transport.calls().empty());
}

TEST(WireClient, ExplainRestoreToPitrRejectsBadTimestamp) {
    CannedTransport transport;
    WireClient client(transport);

    // The one EXPLAIN alternative that validates: a malformed restore
    // timestamp yields the SDK adapter's invalid_argument Error (D-P6c-2).
    ddbsh::RestoreToPitrIn bad;
    bad.source_table_name = "t";
    bad.target_table_name = "t2";
    bad.restore_datetime = "not-a-timestamp";
    ExplainRequest req = bad;
    auto body = client.explain_request(req);
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, Errc::invalid_argument);
    EXPECT_EQ(body.error().message, "Invalid restore timestamp: not-a-timestamp");
    EXPECT_TRUE(transport.calls().empty());
}

// ---- P6c stubs -------------------------------------------------------------

TEST(WireClient, ControlPlaneStubsReportNotImplemented) {
    CannedTransport transport;
    WireClient client(transport);

    EXPECT_EQ(client.describe_table(ddbsh::DescribeTableIn{"t"}).error().code,
              Errc::not_implemented);
    EXPECT_EQ(client.list_tables(ddbsh::ListTablesIn{}).error().code, Errc::not_implemented);
    EXPECT_EQ(client.delete_table(ddbsh::DeleteTableIn{"t"}).error().code, Errc::not_implemented);
    EXPECT_EQ(client.wait_for_table_active("t").error().code, Errc::not_implemented);
    EXPECT_TRUE(transport.calls().empty());
}

// ---- connect records state, makes no call ----------------------------------

TEST(WireClient, ConnectIsNoOpSuccess) {
    CannedTransport transport;
    WireClient client(transport);
    auto r = client.connect("eu-west-1", "");
    EXPECT_TRUE(r.has_value());
    EXPECT_TRUE(transport.calls().empty());
}

}  // namespace
