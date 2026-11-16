// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_serialize_diff_test.cpp — the P6a EXPLAIN-vs-wire differential gate
// (P6-02 §9.1).
//
// The SDK-free ddbsh::wire codec becomes a PRODUCTION serializer the live
// browser transport depends on, so every byte it emits must match what the AWS
// SDK would send. This test asserts, for a corpus covering every field and
// every AttributeValue type of every in-scope data-plane op, that
//
//     ddbsh::wire::serialize(in).body  ==  AwsDDBClient::explain_request(in)
//
// byte-for-byte (explain_request returns the raw SDK SerializePayload(), the
// same bytes the live path would POST). This both proves field coverage
// (nothing the SDK sends is missing or mis-ordered) and locks EXPLAIN
// byte-parity across the CLI and the web app.
//
// It links ddbsh::aws (the AWS SDK), so it is built ONLY when DDBSH_BUILD_AWS is
// ON. It makes NO network call: explain_request just builds the SDK request and
// serializes it offline. It runs as part of the native unit-test suite (which
// is already configured with the SDK), providing a deterministic, backend-free
// fidelity signal exactly like the EXPLAIN unit tests.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/aws/aws_api_guard.hpp"
#include "ddbsh/aws/aws_ddb_client.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/value.hpp"
#include "ddbsh/wire/codec.hpp"

namespace {

using ddbsh::DeleteItemIn;
using ddbsh::ExpressionAttributeNames;
using ddbsh::GetItemIn;
using ddbsh::PutItemIn;
using ddbsh::QueryIn;
using ddbsh::ReturnConsumedCapacity;
using ddbsh::ReturnValue;
using ddbsh::ScanIn;
using ddbsh::UpdateItemIn;
using ddbsh::Value;
using ddbsh::ValueList;
using ddbsh::ValueMap;

// A fixture holding one AWS client (the SDK serialization oracle). Constructing
// the client resolves no credentials eagerly and makes no network call; it is
// only ever asked to serialize (explain_request), never to execute.
class WireDiff : public ::testing::Test {
protected:
    ddbsh::aws::AwsDDBClient client_{"us-east-1", ""};

    // Serialize `in` through both paths and assert byte-identical body + the
    // expected X-Amz-Target operation.
    template <class In>
    void expect_parity(const In& in, const char* op) {
        ddbsh::wire::WireRequest wire = ddbsh::wire::serialize(in);
        EXPECT_EQ(wire.operation, op);
        ddbsh::ExplainRequest req = in;
        auto sdk = client_.explain_request(req);
        ASSERT_TRUE(sdk.has_value()) << "explain_request failed for " << op;
        EXPECT_EQ(wire.body, *sdk) << "wire/SDK body mismatch for " << op;
    }
};

// ---- Value builders --------------------------------------------------------

Value list_of(ValueList elems) {
    Value v;
    v.type = Value::Type::L;
    v.list = std::move(elems);
    return v;
}

Value map_of(ValueMap entries) {
    Value v;
    v.type = Value::Type::M;
    v.map = std::move(entries);
    return v;
}

// An item exercising every AttributeValue type, including nested L/M and empty
// collections (arch §9 supports empty [] / {}).
ValueMap all_types_item() {
    ValueMap m;
    m.emplace_back("s", Value::string_value("hello"));
    m.emplace_back("n", Value::number_value("3.14159"));
    m.emplace_back("b", Value::binary_value("aGVsbG8="));  // base64("hello")
    m.emplace_back("bt", Value::bool_value(true));
    m.emplace_back("bf", Value::bool_value(false));
    m.emplace_back("nul", Value::null_value());
    m.emplace_back("ss", Value::string_set({"x", "y", "z"}));
    m.emplace_back("ns", Value::number_set({"1", "2", "-3"}));
    m.emplace_back("bs", Value::binary_set({"YQ==", "Yg=="}));  // base64("a"),("b")
    m.emplace_back("l", list_of({Value::number_value("4"), Value::string_value("five"),
                                 Value::bool_value(true)}));
    m.emplace_back("m", map_of({{"inner", Value::string_value("v")},
                                {"deep", list_of({Value::number_value("9")})}}));
    m.emplace_back("empty_l", list_of({}));
    m.emplace_back("empty_m", map_of({}));
    return m;
}

// ---- GetItem ---------------------------------------------------------------

TEST_F(WireDiff, GetItemMinimal) {
    GetItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    expect_parity(in, "GetItem");
}

TEST_F(WireDiff, GetItemFull) {
    GetItemIn in;
    in.table_name = "my-table";
    in.key = {{"pk", Value::string_value("a")}, {"sk", Value::number_value("7")}};
    in.projection_expression = "#aaaa1, #aaaa2";
    in.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "name"}, {"#aaaa2", "age"}};
    in.consistent_read = true;
    in.return_consumed_capacity = ReturnConsumedCapacity::total;
    expect_parity(in, "GetItem");
}

// ---- Query -----------------------------------------------------------------

TEST_F(WireDiff, QueryMinimal) {
    QueryIn in;
    in.table_name = "t";
    in.key_condition_expression = "#aaaa1 = :vaaa1";
    in.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "pk"}};
    in.expression_attribute_values = {{":vaaa1", Value::string_value("p")}};
    expect_parity(in, "Query");
}

TEST_F(WireDiff, QueryFull) {
    QueryIn in;
    in.table_name = "t";
    in.index_name = "gsi1";
    in.key_condition_expression = "#aaaa1 = :vaaa1";
    in.filter_expression = "#aaaa2 > :vaaa2";
    in.projection_expression = "#aaaa1, #aaaa2";
    in.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "pk"}, {"#aaaa2", "score"}};
    in.expression_attribute_values = {{":vaaa1", Value::string_value("p")},
                                      {":vaaa2", Value::number_value("10")}};
    in.consistent_read = true;
    in.return_consumed_capacity = ReturnConsumedCapacity::indexes;
    in.exclusive_start_key =
        ValueMap{{"pk", Value::string_value("p")}, {"sk", Value::number_value("5")}};
    expect_parity(in, "Query");
}

// ---- Scan ------------------------------------------------------------------

TEST_F(WireDiff, ScanMinimal) {
    ScanIn in;
    in.table_name = "t";
    expect_parity(in, "Scan");
}

TEST_F(WireDiff, ScanFull) {
    ScanIn in;
    in.table_name = "t";
    in.index_name = "lsi1";
    in.filter_expression = "#aaaa1 = :vaaa1";
    in.projection_expression = "#aaaa1";
    in.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "status"}};
    in.expression_attribute_values = {{":vaaa1", Value::string_value("ok")}};
    in.consistent_read = true;
    in.return_consumed_capacity = ReturnConsumedCapacity::total;
    in.exclusive_start_key = ValueMap{{"pk", Value::string_value("x")}};
    expect_parity(in, "Scan");
}

// ---- PutItem ---------------------------------------------------------------

TEST_F(WireDiff, PutItemInsertConditional) {
    PutItemIn in;
    in.table_name = "t";
    in.item = {{"id", Value::string_value("k1")}, {"v", Value::number_value("1")}};
    in.condition_expression = "attribute_not_exists(#aaaa1)";
    in.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "id"}};
    expect_parity(in, "PutItem");
}

TEST_F(WireDiff, PutItemReplaceReturnValues) {
    PutItemIn in;
    in.table_name = "t";
    in.item = {{"id", Value::string_value("k1")}};
    in.return_values = ReturnValue::all_old;
    in.return_consumed_capacity = ReturnConsumedCapacity::total;
    expect_parity(in, "PutItem");
}

TEST_F(WireDiff, PutItemAllAttributeValueTypes) {
    PutItemIn in;
    in.table_name = "t";
    in.item = all_types_item();
    expect_parity(in, "PutItem");
}

// ---- UpdateItem ------------------------------------------------------------

TEST_F(WireDiff, UpdateItemMinimal) {
    UpdateItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    in.update_expression = "SET #aaaa1 = :vaaa1";
    in.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "v"}};
    in.expression_attribute_values = {{":vaaa1", Value::number_value("2")}};
    expect_parity(in, "UpdateItem");
}

TEST_F(WireDiff, UpdateItemFull) {
    UpdateItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    in.update_expression = "SET #aaaa1 = :vaaa1";
    in.condition_expression = "attribute_exists(#aaaa2)";
    in.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "v"}, {"#aaaa2", "id"}};
    in.expression_attribute_values = {{":vaaa1", Value::number_value("2")}};
    in.return_values = ReturnValue::updated_new;
    in.return_consumed_capacity = ReturnConsumedCapacity::total;
    expect_parity(in, "UpdateItem");
}

// ---- DeleteItem ------------------------------------------------------------

TEST_F(WireDiff, DeleteItemMinimal) {
    DeleteItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    expect_parity(in, "DeleteItem");
}

TEST_F(WireDiff, DeleteItemFull) {
    DeleteItemIn in;
    in.table_name = "t";
    in.key = {{"id", Value::string_value("k1")}};
    in.condition_expression = "attribute_exists(#aaaa1)";
    in.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "id"}};
    in.return_values = ReturnValue::all_old;
    in.return_consumed_capacity = ReturnConsumedCapacity::total;
    expect_parity(in, "DeleteItem");
}

// ---- String escaping -------------------------------------------------------
// Attribute values with characters cJSON escapes by name, a sub-0x20 control
// byte (-> \uXXXX), and multibyte UTF-8 (copied verbatim). A mismatch here
// would mean the escaping diverges from the SDK's printer.
TEST_F(WireDiff, StringEscaping) {
    PutItemIn in;
    in.table_name = "t";
    in.item = {
        {"quote", Value::string_value("a\"b")},
        {"backslash", Value::string_value("a\\b")},
        {"newline", Value::string_value("a\nb")},
        {"tab", Value::string_value("a\tb")},
        {"cr", Value::string_value("a\rb")},
        {"control", Value::string_value(std::string("a\x01\x1f", 3) + "b")},
        {"utf8", Value::string_value("caf\xc3\xa9 \xe2\x9c\x93")},  // "café ✓"
    };
    expect_parity(in, "PutItem");
}

}  // namespace

int main(int argc, char** argv) {
    // One SDK lifecycle for the whole process (the client needs the SDK
    // initialized before construction/serialization).
    ddbsh::aws::AwsApiGuard guard;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
