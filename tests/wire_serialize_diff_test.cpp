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

#include <aws/dynamodb/model/DescribeLimitsRequest.h>
#include <aws/dynamodb/model/ListBackupsRequest.h>
#include <aws/dynamodb/model/ListTablesRequest.h>

#include "ddbsh/aws/aws_api_guard.hpp"
#include "ddbsh/aws/aws_ddb_client.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/table_schema.hpp"
#include "ddbsh/value.hpp"
#include "ddbsh/wire/codec.hpp"

namespace {

using ddbsh::AttributeDef;
using ddbsh::BillingMode;
using ddbsh::BillingModeAndThroughput;
using ddbsh::CreateBackupIn;
using ddbsh::CreateTableIn;
using ddbsh::DeleteBackupIn;
using ddbsh::DeleteItemIn;
using ddbsh::DeleteTableIn;
using ddbsh::DescribeBackupIn;
using ddbsh::DescribeContinuousBackupsIn;
using ddbsh::DescribeLimitsIn;
using ddbsh::DescribeTableIn;
using ddbsh::DescribeTableReplicaAutoScalingIn;
using ddbsh::DescribeTimeToLiveIn;
using ddbsh::ExpressionAttributeNames;
using ddbsh::GetItemIn;
using ddbsh::Gsi;
using ddbsh::GsiUpdate;
using ddbsh::KeySchema;
using ddbsh::ListBackupsIn;
using ddbsh::ListTablesIn;
using ddbsh::Lsi;
using ddbsh::OnDemandThroughput;
using ddbsh::Projection;
using ddbsh::ProjectionType;
using ddbsh::ProvisionedThroughputOverride;
using ddbsh::PutItemIn;
using ddbsh::QueryIn;
using ddbsh::ReplicaGsi;
using ddbsh::ReplicaUpdate;
using ddbsh::RestoreFromBackupIn;
using ddbsh::RestoreToPitrIn;
using ddbsh::ReturnConsumedCapacity;
using ddbsh::ReturnValue;
using ddbsh::ScalarType;
using ddbsh::ScanIn;
using ddbsh::SearchSchemaElem;
using ddbsh::SearchSchemaKind;
using ddbsh::SearchVectorsIn;
using ddbsh::SseSpec;
using ddbsh::SseType;
using ddbsh::StreamSpec;
using ddbsh::StreamViewType;
using ddbsh::TableClass;
using ddbsh::Tag;
using ddbsh::TransactConditionCheck;
using ddbsh::TransactDelete;
using ddbsh::TransactGetItemsIn;
using ddbsh::TransactPut;
using ddbsh::TransactUpdate;
using ddbsh::TransactWriteItemsIn;
using ddbsh::UpdateContinuousBackupsIn;
using ddbsh::UpdateItemIn;
using ddbsh::UpdateTableIn;
using ddbsh::UpdateTimeToLiveIn;
using ddbsh::Value;
using ddbsh::ValueList;
using ddbsh::ValueMap;
using ddbsh::VectorDistance;
using ddbsh::VectorIndexDef;
using ddbsh::VectorIndexUpdateDef;
using ddbsh::WarmThroughput;

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

    // The three SHOW ops (DescribeLimits, ListTables, ListBackups) are NOT in
    // the ExplainRequest variant (the SHOW commands have no EXPLAIN), so they
    // cannot route through explain_request. Pin them against a directly-built
    // SDK request serialized with the SDK's own SerializePayload() — the same
    // byte oracle, reached without the EXPLAIN seam.
    template <class In, class SdkRequest>
    void expect_sdk_parity(const In& in, const char* op, const SdkRequest& sdk_request) {
        ddbsh::wire::WireRequest wire = ddbsh::wire::serialize(in);
        EXPECT_EQ(wire.operation, op);
        const std::string sdk(sdk_request.SerializePayload().c_str());
        EXPECT_EQ(wire.body, sdk) << "wire/SDK body mismatch for " << op;
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

// ===========================================================================
// P6c — control-plane, backup/restore, transaction, and vector ops.
// ===========================================================================

// ---- helpers ---------------------------------------------------------------

KeySchema hash_key(const std::string& h) {
    KeySchema ks;
    ks.hash.push_back(h);
    return ks;
}

KeySchema hash_range(const std::string& h, const std::string& r) {
    KeySchema ks;
    ks.hash.push_back(h);
    ks.range.push_back(r);
    return ks;
}

Projection project_all() {
    return Projection{ProjectionType::all, {}};
}

Projection project_include(std::vector<std::string> attrs) {
    return Projection{ProjectionType::include, std::move(attrs)};
}

BillingModeAndThroughput provisioned(long long rcu, long long wcu) {
    BillingModeAndThroughput b;
    b.mode = BillingMode::provisioned;
    b.rcu = rcu;
    b.wcu = wcu;
    return b;
}

BillingModeAndThroughput on_demand(std::optional<long long> r, std::optional<long long> w) {
    BillingModeAndThroughput b;
    b.mode = BillingMode::pay_per_request;
    b.od.max_read_request_units = r;
    b.od.max_write_request_units = w;
    return b;
}

// TransactWriteItems is the one op whose SDK body cannot byte-match any
// SDK-free serializer: TransactWriteItemsRequest auto-populates a RANDOM
// ClientRequestToken (an idempotency UUID) that SerializePayload always emits.
// The wire serializer omits the token (D-P6c-9: non-golden-affecting — no
// golden pins it, and the SDK value is random, so byte-parity is impossible by
// construction; the live wire path simply does not send an idempotency token).
// The differential gate strips the token member from the SDK body before
// comparing, so it still pins the rest of the TransactItems body byte-for-byte.
std::string strip_client_request_token(std::string s) {
    const std::string key = "\"ClientRequestToken\"";
    const auto kpos = s.find(key);
    if (kpos == std::string::npos) {
        return s;
    }
    const auto comma = s.rfind(',', kpos);       // the member separator before it
    const auto colon_tab = s.find('\t', kpos);   // the '\t' after the ':'
    const auto vstart = s.find('"', colon_tab);  // opening quote of the uuid value
    const auto vend = s.find('"', vstart + 1);   // closing quote
    s.erase(comma, (vend + 1) - comma);
    return s;
}

// ---- CreateTable -----------------------------------------------------------

TEST_F(WireDiff, CreateTableDefaultOnDemand) {
    CreateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"id", ScalarType::s}};
    in.key_schema = hash_key("id");
    expect_parity(in, "CreateTable");
}

TEST_F(WireDiff, CreateTableProvisionedWithIndexes) {
    CreateTableIn in;
    in.table_name = "my-table";
    in.attribute_definitions = {{"pk", ScalarType::s},
                                {"sk", ScalarType::n},
                                {"gpk", ScalarType::s},
                                {"lsk", ScalarType::n}};
    in.key_schema = hash_range("pk", "sk");
    in.billing = provisioned(5, 7);
    Gsi gsi;
    gsi.name = "gsi1";
    gsi.key_schema = hash_key("gpk");
    gsi.projection = project_include({"a", "b"});
    gsi.billing = provisioned(3, 4);
    in.gsis.push_back(gsi);
    Lsi lsi;
    lsi.name = "lsi1";
    lsi.key_schema = hash_range("pk", "lsk");
    lsi.projection = project_all();
    in.lsis.push_back(lsi);
    expect_parity(in, "CreateTable");
}

TEST_F(WireDiff, CreateTableOnDemandThroughputSentinels) {
    CreateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"id", ScalarType::s}};
    in.key_schema = hash_key("id");
    in.billing = on_demand(-1, -1);  // THROUGHPUT UNLIMITED
    expect_parity(in, "CreateTable");
}

TEST_F(WireDiff, CreateTableBareOnDemandEmptyThroughput) {
    CreateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"id", ScalarType::s}};
    in.key_schema = hash_key("id");
    in.billing = on_demand(std::nullopt, std::nullopt);  // bare ON DEMAND -> {}
    expect_parity(in, "CreateTable");
}

TEST_F(WireDiff, CreateTableOnDemandLargeMaxUnits) {
    // Pin the integer number path at a realistic on-demand maximum (above the
    // small 3/4/5/7 values the rest of the corpus uses), exercising cJSON's
    // integer formatting for a multi-digit value.
    CreateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"id", ScalarType::s}};
    in.key_schema = hash_key("id");
    in.billing = on_demand(40000, 80000);
    expect_parity(in, "CreateTable");
}

TEST_F(WireDiff, CreateTableFullFeatured) {
    CreateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"id", ScalarType::b}};
    in.key_schema = hash_key("id");
    in.billing = provisioned(1, 1);
    in.warm = WarmThroughput{100, 200};
    in.stream = StreamSpec{true, StreamViewType::new_and_old_images};
    SseSpec sse;
    sse.enabled = true;
    sse.sse_type = SseType::kms;
    sse.kms_master_key_id = "alias/mykey";
    in.sse = sse;
    in.table_class = TableClass::standard_infrequent_access;
    in.deletion_protection = true;
    in.tags = {{"team", "ddbsh"}, {"env", "test"}};
    expect_parity(in, "CreateTable");
}

TEST_F(WireDiff, CreateTableCompositeGsiKey) {
    // GSI composite keys: up to 4 HASH + 4 RANGE (settled platform fact).
    CreateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"pk", ScalarType::s}, {"a", ScalarType::s}, {"b", ScalarType::s},
                                {"c", ScalarType::s},  {"d", ScalarType::s}, {"e", ScalarType::s}};
    in.key_schema = hash_key("pk");
    Gsi gsi;
    gsi.name = "mpgsi";
    gsi.key_schema.hash = {"a", "b", "c"};
    gsi.key_schema.range = {"d", "e"};
    gsi.projection = project_all();
    in.gsis.push_back(gsi);
    expect_parity(in, "CreateTable");
}

TEST_F(WireDiff, CreateTableVectorIndex) {
    CreateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"id", ScalarType::s}, {"cat", ScalarType::s}};
    in.key_schema = hash_key("id");
    VectorIndexDef vi;
    vi.index_name = "vidx";
    vi.vector_attr = "embedding";
    vi.dimensions = 128;
    vi.distance = VectorDistance::cosine;
    vi.search_schema = {{"cat", SearchSchemaKind::hash}};
    vi.projection = project_include({"title"});
    in.vector_indexes.push_back(vi);
    expect_parity(in, "CreateTable");
}

// ---- UpdateTable -----------------------------------------------------------

TEST_F(WireDiff, UpdateTableBillingProvisioned) {
    UpdateTableIn in;
    in.table_name = "t";
    in.billing = provisioned(10, 20);
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableBillingOnDemand) {
    UpdateTableIn in;
    in.table_name = "t";
    in.billing = on_demand(50, 60);
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableGsiCreate) {
    UpdateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"gpk", ScalarType::s}};
    GsiUpdate gu;
    gu.action = GsiUpdate::Action::create;
    gu.index_name = "gsi1";
    gu.key_schema = hash_key("gpk");
    gu.projection = project_all();
    gu.billing = provisioned(2, 3);
    in.gsi_updates.push_back(gu);
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableGsiUpdate) {
    UpdateTableIn in;
    in.table_name = "t";
    GsiUpdate gu;
    gu.action = GsiUpdate::Action::update;
    gu.index_name = "gsi1";
    gu.billing = provisioned(9, 8);
    in.gsi_updates.push_back(gu);
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableGsiDrop) {
    UpdateTableIn in;
    in.table_name = "t";
    GsiUpdate gu;
    gu.action = GsiUpdate::Action::drop;
    gu.index_name = "gsi1";
    in.gsi_updates.push_back(gu);
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableStreamSseClassProtection) {
    UpdateTableIn in;
    in.table_name = "t";
    in.stream = StreamSpec{true, StreamViewType::keys_only};
    SseSpec sse;
    sse.enabled = false;
    in.sse = sse;
    in.table_class = TableClass::standard;
    in.deletion_protection = false;
    in.warm = WarmThroughput{1, 2};
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableReplicaAdd) {
    UpdateTableIn in;
    in.table_name = "t";
    ReplicaUpdate r;
    r.action = ReplicaUpdate::Action::add;
    r.region = "us-west-2";
    r.throughput_override = ProvisionedThroughputOverride{5};
    r.replica_gsis = {ReplicaGsi{"gsi1", ProvisionedThroughputOverride{3}}};
    in.replica = r;
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableReplicaUpdateDefaultTableClass) {
    UpdateTableIn in;
    in.table_name = "t";
    ReplicaUpdate r;
    r.action = ReplicaUpdate::Action::update;
    r.region = "eu-west-1";
    in.replica = r;
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableReplicaDrop) {
    UpdateTableIn in;
    in.table_name = "t";
    ReplicaUpdate r;
    r.action = ReplicaUpdate::Action::drop;
    r.region = "ap-south-1";
    in.replica = r;
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableVectorIndexCreate) {
    UpdateTableIn in;
    in.table_name = "t";
    in.attribute_definitions = {{"cat", ScalarType::s}};
    VectorIndexDef vi;
    vi.index_name = "vidx";
    vi.vector_attr = "embedding";
    vi.dimensions = 64;
    vi.distance = VectorDistance::euclidean;
    vi.search_schema = {{"cat", SearchSchemaKind::inline_filter}};
    vi.projection = project_all();
    VectorIndexUpdateDef vu;
    vu.create = vi;
    in.vector_index_updates.push_back(vu);
    expect_parity(in, "UpdateTable");
}

TEST_F(WireDiff, UpdateTableVectorIndexDrop) {
    UpdateTableIn in;
    in.table_name = "t";
    VectorIndexUpdateDef vu;
    vu.drop = "vidx";
    in.vector_index_updates.push_back(vu);
    expect_parity(in, "UpdateTable");
}

// ---- DeleteTable / TTL / PITR ----------------------------------------------

TEST_F(WireDiff, DeleteTable) {
    DeleteTableIn in;
    in.table_name = "t";
    expect_parity(in, "DeleteTable");
}

TEST_F(WireDiff, UpdateTimeToLiveEnable) {
    UpdateTimeToLiveIn in;
    in.table_name = "t";
    in.enabled = true;
    in.attribute_name = "expires_at";
    expect_parity(in, "UpdateTimeToLive");
}

TEST_F(WireDiff, UpdateTimeToLiveDisable) {
    UpdateTimeToLiveIn in;
    in.table_name = "t";
    in.enabled = false;
    in.attribute_name = "expires_at";
    expect_parity(in, "UpdateTimeToLive");
}

TEST_F(WireDiff, UpdateContinuousBackupsEnable) {
    UpdateContinuousBackupsIn in;
    in.table_name = "t";
    in.enabled = true;
    expect_parity(in, "UpdateContinuousBackups");
}

// ---- Describe family -------------------------------------------------------

TEST_F(WireDiff, DescribeTable) {
    DescribeTableIn in;
    in.table_name = "t";
    expect_parity(in, "DescribeTable");
}

TEST_F(WireDiff, DescribeTimeToLive) {
    DescribeTimeToLiveIn in;
    in.table_name = "t";
    expect_parity(in, "DescribeTimeToLive");
}

TEST_F(WireDiff, DescribeContinuousBackups) {
    DescribeContinuousBackupsIn in;
    in.table_name = "t";
    expect_parity(in, "DescribeContinuousBackups");
}

TEST_F(WireDiff, DescribeTableReplicaAutoScaling) {
    DescribeTableReplicaAutoScalingIn in;
    in.table_name = "t";
    expect_parity(in, "DescribeTableReplicaAutoScaling");
}

// ---- Backup / Restore ------------------------------------------------------

TEST_F(WireDiff, CreateBackup) {
    CreateBackupIn in;
    in.table_name = "t";
    in.backup_name = "bk1";
    expect_parity(in, "CreateBackup");
}

TEST_F(WireDiff, DeleteBackup) {
    DeleteBackupIn in;
    in.backup_arn = "arn:aws:dynamodb:us-east-1:123:table/t/backup/01";
    expect_parity(in, "DeleteBackup");
}

TEST_F(WireDiff, DescribeBackup) {
    DescribeBackupIn in;
    in.backup_arn = "arn:aws:dynamodb:us-east-1:123:table/t/backup/01";
    expect_parity(in, "DescribeBackup");
}

TEST_F(WireDiff, RestoreFromBackup) {
    RestoreFromBackupIn in;
    in.target_table_name = "t2";
    in.backup_arn = "arn:aws:dynamodb:us-east-1:123:table/t/backup/01";
    expect_parity(in, "RestoreTableFromBackup");
}

TEST_F(WireDiff, RestoreToPitrWholeSecond) {
    RestoreToPitrIn in;
    in.source_table_name = "t";
    in.target_table_name = "t2";
    in.restore_datetime = "2023-11-14T22:13:20Z";
    expect_parity(in, "RestoreTableToPointInTime");
}

TEST_F(WireDiff, RestoreToPitrMilliseconds) {
    // The RestoreDateTime double exercises cJSON's readable-number printing for
    // a fractional value (SecondsWithMSPrecision adds milliseconds/1000). The
    // ".500Z" form is accepted by the SDK's ISO_8601DateParser (>= 3 fractional
    // digits) even though ddbsh's T_TIMESTAMP lexer only ever produces the
    // whole-second form above.
    RestoreToPitrIn in;
    in.source_table_name = "t";
    in.target_table_name = "t2";
    in.restore_datetime = "2024-02-29T12:34:56.500Z";
    expect_parity(in, "RestoreTableToPointInTime");
}

// ---- Transactions ----------------------------------------------------------

TEST_F(WireDiff, TransactGetItems) {
    TransactGetItemsIn in;
    ddbsh::TransactGet g1;
    g1.table_name = "t";
    g1.key = {{"id", Value::string_value("k1")}};
    ddbsh::TransactGet g2;
    g2.table_name = "u";
    g2.key = {{"pk", Value::string_value("a")}, {"sk", Value::number_value("2")}};
    g2.projection_expression = "#aaaa1";
    g2.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "name"}};
    in.items = {g1, g2};
    expect_parity(in, "TransactGetItems");
}

TEST_F(WireDiff, TransactWriteItemsAllKinds) {
    TransactWriteItemsIn in;
    TransactPut put;
    put.table_name = "t";
    put.item = {{"id", Value::string_value("k1")}, {"v", Value::number_value("1")}};
    put.condition_expression = "attribute_not_exists(#aaaa1)";
    put.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "id"}};
    TransactUpdate upd;
    upd.table_name = "t";
    upd.key = {{"id", Value::string_value("k2")}};
    upd.update_expression = "SET #aaaa1 = :vaaa1";
    upd.condition_expression = "attribute_exists(#aaaa1)";
    upd.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "v"}};
    upd.expression_attribute_values = {{":vaaa1", Value::number_value("9")}};
    TransactDelete del;
    del.table_name = "t";
    del.key = {{"id", Value::string_value("k3")}};
    TransactConditionCheck cc;
    cc.table_name = "t";
    cc.key = {{"id", Value::string_value("k4")}};
    cc.condition_expression = "attribute_exists(#aaaa1)";
    cc.expression_attribute_names = ExpressionAttributeNames{{"#aaaa1", "id"}};
    in.items = {put, upd, del, cc};
    // The SDK auto-adds a random ClientRequestToken; strip it before comparing
    // (D-P6c-9). The rest of the TransactItems body is pinned byte-for-byte.
    ddbsh::wire::WireRequest wire = ddbsh::wire::serialize(in);
    EXPECT_EQ(wire.operation, "TransactWriteItems");
    ddbsh::ExplainRequest req = in;
    auto sdk = client_.explain_request(req);
    ASSERT_TRUE(sdk.has_value());
    EXPECT_EQ(wire.body, strip_client_request_token(*sdk));
}

// ---- SearchVectors ---------------------------------------------------------

TEST_F(WireDiff, SearchVectorsMinimal) {
    SearchVectorsIn in;
    in.table_name = "t";
    in.index_name = "vidx";
    in.search_vector = ValueList{Value::number_value("0.1"), Value::number_value("0.2"),
                                 Value::number_value("0.3")};
    in.topk = 5;
    expect_parity(in, "SearchVectors");
}

TEST_F(WireDiff, SearchVectorsFull) {
    SearchVectorsIn in;
    in.table_name = "t";
    in.index_name = "vidx";
    in.search_vector = ValueList{Value::number_value("1"), Value::number_value("2")};
    in.topk = 10;
    in.search_condition_expression = "#aaaa1 = :vaaa1";
    in.projection_expression = "#aaaa2";
    in.expression_attribute_names =
        ExpressionAttributeNames{{"#aaaa1", "cat"}, {"#aaaa2", "title"}};
    in.expression_attribute_values = {{":vaaa1", Value::string_value("x")}};
    in.return_consumed_capacity = ReturnConsumedCapacity::total;
    expect_parity(in, "SearchVectors");
}

// ---- SHOW ops (not in ExplainRequest; pinned against a built SDK request) --

TEST_F(WireDiff, DescribeLimitsEmptyBody) {
    expect_sdk_parity(DescribeLimitsIn{}, "DescribeLimits",
                      Aws::DynamoDB::Model::DescribeLimitsRequest{});
}

TEST_F(WireDiff, ListTablesEmptyBody) {
    expect_sdk_parity(ListTablesIn{}, "ListTables", Aws::DynamoDB::Model::ListTablesRequest{});
}

TEST_F(WireDiff, ListBackupsMinimal) {
    ListBackupsIn in;  // limit defaults to 100
    Aws::DynamoDB::Model::ListBackupsRequest sdk;
    sdk.SetLimit(100);
    expect_sdk_parity(in, "ListBackups", sdk);
}

TEST_F(WireDiff, ListBackupsFull) {
    ListBackupsIn in;
    in.table_name = "t";
    in.backup_type = "USER";
    in.limit = 25;
    in.exclusive_start_backup_arn = "arn:aws:dynamodb:us-east-1:123:table/t/backup/01";
    Aws::DynamoDB::Model::ListBackupsRequest sdk;
    sdk.SetTableName("t");
    sdk.SetLimit(25);
    sdk.SetExclusiveStartBackupArn("arn:aws:dynamodb:us-east-1:123:table/t/backup/01");
    sdk.SetBackupType(
        Aws::DynamoDB::Model::BackupTypeFilterMapper::GetBackupTypeFilterForName("USER"));
    expect_sdk_parity(in, "ListBackups", sdk);
}

}  // namespace

int main(int argc, char** argv) {
    // One SDK lifecycle for the whole process (the client needs the SDK
    // initialized before construction/serialization).
    ddbsh::aws::AwsApiGuard guard;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
