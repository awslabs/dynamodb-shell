// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_parse_control_test.cpp — the P6c-b control-plane / backup-restore /
// transaction / vector response parsers (P6-02 §4; the design §6.2).
//
// Hand-authored canonical AWS-JSON-1.0 response bodies asserted field-by-field
// against the AWS adapter's documented response assembly (aws/aws_ddb_client.cpp,
// the fidelity oracle): each `*HasBeenSet()` edge, the enum-NAME-string reads,
// the epoch-seconds -> ISO-8601 datetime formatting, the token-only `parse_ack`
// ops (success body ignored; `__type` -> mapped Error), and the malformed-body
// failure paths (tenet 6). Datetimes are checked against epoch_to_iso8601 (the
// same codec the parser uses — pinned independently by wire_datetime_test), so
// this test proves the parser threads the timestamp, not the codec.

#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/table_schema.hpp"
#include "ddbsh/wire/codec.hpp"
#include "wire/datetime.hpp"

namespace {

using ddbsh::Errc;
using ddbsh::ProjectionType;
using ddbsh::ScalarType;
using ddbsh::SearchSchemaKind;
using ddbsh::VectorDistance;
using ddbsh::wire::epoch_to_iso8601;

// A convenient fixed instant used across the datetime fixtures.
constexpr double kEpoch = 1700000000.0;  // 2023-11-14T22:13:20Z
std::string iso() {
    return epoch_to_iso8601(kEpoch);
}

// ---- DescribeTable ---------------------------------------------------------

TEST(WireParseControl, DescribeTableProvisionedFull) {
    const std::string body = R"({"Table":{
        "TableName":"t",
        "KeySchema":[{"AttributeName":"a","KeyType":"HASH"},{"AttributeName":"b","KeyType":"RANGE"}],
        "AttributeDefinitions":[{"AttributeName":"a","AttributeType":"S"},{"AttributeName":"b","AttributeType":"N"}],
        "TableStatus":"ACTIVE",
        "TableId":"id-1",
        "TableArn":"arn:t",
        "CreationDateTime":1700000000,
        "TableSizeBytes":4096,
        "ItemCount":12,
        "DeletionProtectionEnabled":true,
        "BillingModeSummary":{"BillingMode":"PROVISIONED"},
        "ProvisionedThroughput":{"ReadCapacityUnits":5,"WriteCapacityUnits":7},
        "TableClassSummary":{"TableClass":"STANDARD_INFREQUENT_ACCESS"}
    }})";
    auto out = ddbsh::wire::parse_describe_table(body);
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().message);
    EXPECT_EQ(out->table_name, "t");
    ASSERT_EQ(out->key_schema.hash_keys.size(), 1u);
    EXPECT_EQ(out->key_schema.hash_keys[0], "a");
    ASSERT_EQ(out->key_schema.range_keys.size(), 1u);
    EXPECT_EQ(out->key_schema.range_keys[0], "b");
    ASSERT_EQ(out->attribute_definitions.size(), 2u);
    EXPECT_EQ(out->attribute_definitions[0].name, "a");
    EXPECT_EQ(out->attribute_definitions[0].type, ScalarType::s);
    EXPECT_EQ(out->attribute_definitions[1].type, ScalarType::n);
    EXPECT_EQ(out->table_status, "ACTIVE");
    EXPECT_EQ(out->table_id, "id-1");
    EXPECT_EQ(out->table_arn, "arn:t");
    EXPECT_EQ(out->creation_date_time, iso());
    EXPECT_EQ(out->table_size_bytes, 4096);
    EXPECT_EQ(out->item_count, 12);
    EXPECT_TRUE(out->deletion_protection_enabled);
    ASSERT_TRUE(out->billing_mode_summary.has_value());
    EXPECT_EQ(*out->billing_mode_summary, "PROVISIONED");
    EXPECT_TRUE(out->provisioned_throughput_set);
    EXPECT_EQ(out->provisioned_read_capacity_units, 5);
    EXPECT_EQ(out->provisioned_write_capacity_units, 7);
    ASSERT_TRUE(out->table_class_summary.has_value());
    EXPECT_EQ(*out->table_class_summary, "STANDARD_INFREQUENT_ACCESS");
    // Absent optionals stay unset.
    EXPECT_FALSE(out->on_demand_throughput_set);
    EXPECT_FALSE(out->warm_throughput.has_value());
    EXPECT_FALSE(out->stream_specification_set);
    EXPECT_FALSE(out->stream_view_type.has_value());
    EXPECT_FALSE(out->sse_status.has_value());
}

TEST(WireParseControl, DescribeTableOnDemandStreamSseWarmAndIndexes) {
    const std::string body = R"({"Table":{
        "TableName":"t",
        "KeySchema":[{"AttributeName":"a","KeyType":"HASH"}],
        "TableStatus":"ACTIVE",
        "OnDemandThroughput":{"MaxReadRequestUnits":40000},
        "WarmThroughput":{"ReadUnitsPerSecond":100,"WriteUnitsPerSecond":50,"Status":"ACTIVE"},
        "StreamSpecification":{"StreamEnabled":true,"StreamViewType":"NEW_AND_OLD_IMAGES"},
        "SSEDescription":{"Status":"ENABLED"},
        "LocalSecondaryIndexes":[{"IndexName":"l1","KeySchema":[{"AttributeName":"a","KeyType":"HASH"},{"AttributeName":"c","KeyType":"RANGE"}]}],
        "GlobalSecondaryIndexes":[{"IndexName":"g1","KeySchema":[{"AttributeName":"d","KeyType":"HASH"}],"Projection":{"ProjectionType":"INCLUDE","NonKeyAttributes":["x","y"]},"IndexStatus":"ACTIVE","Backfilling":false,"ProvisionedThroughput":{"ReadCapacityUnits":3,"WriteCapacityUnits":4}}],
        "VectorIndexes":[{"IndexName":"v1","VectorAttribute":{"AttributeName":"emb"},"Dimensions":3,"DistanceFunction":"COSINE","SearchSchema":[{"AttributeName":"a","SearchSchemaElementType":"HASH"}],"Projection":{"ProjectionType":"ALL"},"IndexStatus":"CREATING"}]
    }})";
    auto out = ddbsh::wire::parse_describe_table(body);
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().message);

    EXPECT_TRUE(out->on_demand_throughput_set);
    EXPECT_EQ(out->on_demand_max_read_request_units, 40000);
    EXPECT_EQ(out->on_demand_max_write_request_units, -1);  // unset dimension => -1

    ASSERT_TRUE(out->warm_throughput.has_value());
    EXPECT_EQ(out->warm_throughput->read_units_per_second, 100);
    EXPECT_EQ(out->warm_throughput->write_units_per_second, 50);
    EXPECT_EQ(out->warm_throughput->status, "ACTIVE");

    EXPECT_TRUE(out->stream_specification_set);
    ASSERT_TRUE(out->stream_view_type.has_value());
    EXPECT_EQ(*out->stream_view_type, "NEW_AND_OLD_IMAGES");
    ASSERT_TRUE(out->sse_status.has_value());
    EXPECT_EQ(*out->sse_status, "ENABLED");

    ASSERT_EQ(out->local_secondary_indexes.size(), 1u);
    EXPECT_EQ(out->local_secondary_indexes[0].index_name, "l1");
    EXPECT_EQ(out->local_secondary_indexes[0].key_schema.range_keys.at(0), "c");
    EXPECT_FALSE(out->local_secondary_indexes[0].projection_set);  // LSI: name+keyschema only

    ASSERT_EQ(out->global_secondary_indexes.size(), 1u);
    const auto& g = out->global_secondary_indexes[0];
    EXPECT_EQ(g.index_name, "g1");
    EXPECT_TRUE(g.projection_set);
    EXPECT_EQ(g.projection.type, ProjectionType::include);
    EXPECT_EQ(g.projection.non_key_attributes, (std::vector<std::string>{"x", "y"}));
    EXPECT_EQ(g.index_status, "ACTIVE");
    EXPECT_EQ(g.provisioned_read_capacity_units, 3);
    EXPECT_EQ(g.provisioned_write_capacity_units, 4);

    ASSERT_EQ(out->vector_indexes.size(), 1u);
    const auto& v = out->vector_indexes[0];
    EXPECT_EQ(v.index_name, "v1");
    EXPECT_EQ(v.vector_attr, "emb");
    EXPECT_EQ(v.dimensions, 3);
    EXPECT_EQ(v.distance, VectorDistance::cosine);
    ASSERT_EQ(v.search_schema.size(), 1u);
    EXPECT_EQ(v.search_schema[0].attr, "a");
    EXPECT_EQ(v.search_schema[0].kind, SearchSchemaKind::hash);
    EXPECT_EQ(v.index_status, "CREATING");
}

TEST(WireParseControl, DescribeTableStreamDisabledSetsFlagNotViewType) {
    const std::string body = R"({"Table":{"TableName":"t","TableStatus":"ACTIVE",
        "StreamSpecification":{"StreamEnabled":false}}})";
    auto out = ddbsh::wire::parse_describe_table(body);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->stream_specification_set);
    EXPECT_FALSE(out->stream_view_type.has_value());
}

TEST(WireParseControl, DescribeTableMissingTableIsMalformed) {
    auto out = ddbsh::wire::parse_describe_table(R"({"NotTable":{}})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
}

TEST(WireParseControl, DescribeTableErrorDocumentMapped) {
    auto out = ddbsh::wire::parse_describe_table(
        R"({"__type":"x#ResourceNotFoundException","message":"Requested resource not found"})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    ASSERT_TRUE(out.error().service_error_name.has_value());
    EXPECT_EQ(*out.error().service_error_name, "ResourceNotFoundException");
    EXPECT_EQ(out.error().message, "Requested resource not found");
}

// ---- DescribeTimeToLive ----------------------------------------------------

TEST(WireParseControl, DescribeTimeToLive) {
    auto out = ddbsh::wire::parse_describe_time_to_live(
        R"({"TimeToLiveDescription":{"TimeToLiveStatus":"ENABLED","AttributeName":"ttl"}})");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->status, "ENABLED");
    EXPECT_EQ(out->attribute_name, "ttl");
}

// ---- DescribeContinuousBackups (PITR) --------------------------------------

TEST(WireParseControl, DescribeContinuousBackupsEnabledHasDatetimes) {
    const std::string body = R"({"ContinuousBackupsDescription":{
        "ContinuousBackupsStatus":"ENABLED",
        "PointInTimeRecoveryDescription":{
            "PointInTimeRecoveryStatus":"ENABLED",
            "EarliestRestorableDateTime":1700000000,
            "LatestRestorableDateTime":1700000000}}})";
    auto out = ddbsh::wire::parse_describe_continuous_backups(body);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->pitr_status, "ENABLED");
    EXPECT_EQ(out->earliest_restorable_date_time, iso());
    EXPECT_EQ(out->latest_restorable_date_time, iso());
}

TEST(WireParseControl, DescribeContinuousBackupsDisabledNoDatetimes) {
    const std::string body = R"({"ContinuousBackupsDescription":{
        "PointInTimeRecoveryDescription":{"PointInTimeRecoveryStatus":"DISABLED"}}})";
    auto out = ddbsh::wire::parse_describe_continuous_backups(body);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->pitr_status, "DISABLED");
    EXPECT_TRUE(out->earliest_restorable_date_time.empty());
    EXPECT_TRUE(out->latest_restorable_date_time.empty());
}

// ---- DescribeTableReplicaAutoScaling ---------------------------------------

TEST(WireParseControl, ReplicaAutoScalingManyReplicas) {
    const std::string body = R"({"TableAutoScalingDescription":{"Replicas":[
        {"RegionName":"us-east-1","ReplicaStatus":"ACTIVE"},
        {"RegionName":"eu-west-1","ReplicaStatus":"CREATING"}]}})";
    auto out = ddbsh::wire::parse_describe_table_replica_auto_scaling(body);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->replicas.size(), 2u);
    EXPECT_EQ(out->replicas[0].region_name, "us-east-1");
    EXPECT_EQ(out->replicas[0].replica_status, "ACTIVE");
    EXPECT_EQ(out->replicas[1].region_name, "eu-west-1");
    EXPECT_EQ(out->replicas[1].replica_status, "CREATING");
}

TEST(WireParseControl, ReplicaAutoScalingEmpty) {
    auto out = ddbsh::wire::parse_describe_table_replica_auto_scaling(
        R"({"TableAutoScalingDescription":{"Replicas":[]}})");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->replicas.empty());
}

// ---- DescribeLimits --------------------------------------------------------

TEST(WireParseControl, DescribeLimits) {
    const std::string body = R"({
        "AccountMaxReadCapacityUnits":80000,
        "AccountMaxWriteCapacityUnits":80000,
        "TableMaxReadCapacityUnits":40000,
        "TableMaxWriteCapacityUnits":40000})";
    auto out = ddbsh::wire::parse_describe_limits(body);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->account_max_rcu, 80000);
    EXPECT_EQ(out->account_max_wcu, 80000);
    EXPECT_EQ(out->table_max_rcu, 40000);
    EXPECT_EQ(out->table_max_wcu, 40000);
}

// ---- DescribeBackup --------------------------------------------------------

TEST(WireParseControl, DescribeBackupProvisioned) {
    const std::string body = R"({"BackupDescription":{
        "SourceTableDetails":{
            "TableName":"t","TableId":"id-1","TableArn":"arn:t",
            "TableSizeBytes":2048,"ItemCount":9,
            "KeySchema":[{"AttributeName":"a","KeyType":"HASH"}],
            "TableCreationDateTime":1700000000,
            "BillingMode":"PROVISIONED",
            "ProvisionedThroughput":{"ReadCapacityUnits":5,"WriteCapacityUnits":6}},
        "BackupDetails":{
            "BackupName":"bk","BackupArn":"arn:bk","BackupSizeBytes":1024,
            "BackupStatus":"AVAILABLE","BackupType":"USER",
            "BackupCreationDateTime":1700000000,"BackupExpiryDateTime":1700000000}}})";
    auto out = ddbsh::wire::parse_describe_backup(body);
    ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().message);
    EXPECT_EQ(out->source_table_name, "t");
    EXPECT_EQ(out->source_table_id, "id-1");
    EXPECT_EQ(out->source_table_arn, "arn:t");
    EXPECT_EQ(out->table_size_bytes, 2048);
    EXPECT_EQ(out->item_count, 9);
    ASSERT_EQ(out->key_schema.size(), 1u);
    EXPECT_EQ(out->key_schema[0].key_type, "HASH");
    EXPECT_EQ(out->key_schema[0].attribute_name, "a");
    EXPECT_EQ(out->table_creation_datetime, iso());
    ASSERT_TRUE(out->billing_mode.has_value());
    EXPECT_EQ(*out->billing_mode, "PROVISIONED");
    ASSERT_TRUE(out->provisioned.has_value());
    EXPECT_EQ(out->provisioned->first, 5);
    EXPECT_EQ(out->provisioned->second, 6);
    EXPECT_EQ(out->backup_name, "bk");
    EXPECT_EQ(out->backup_arn, "arn:bk");
    EXPECT_EQ(out->backup_size_bytes, 1024);
    EXPECT_EQ(out->backup_status, "AVAILABLE");
    EXPECT_EQ(out->backup_type, "USER");
    EXPECT_EQ(out->backup_creation_datetime, iso());
    EXPECT_EQ(out->backup_expiry_datetime, iso());
}

TEST(WireParseControl, DescribeBackupOnDemandLeavesBillingAndProvisionedUnset) {
    const std::string body = R"({"BackupDescription":{
        "SourceTableDetails":{"TableName":"t","KeySchema":[],"TableCreationDateTime":1700000000},
        "BackupDetails":{"BackupName":"bk","BackupArn":"arn:bk","BackupStatus":"AVAILABLE",
            "BackupType":"USER","BackupCreationDateTime":1700000000,"BackupExpiryDateTime":1700000000}}})";
    auto out = ddbsh::wire::parse_describe_backup(body);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->billing_mode.has_value());
    EXPECT_FALSE(out->provisioned.has_value());
}

// ---- ListBackups -----------------------------------------------------------

TEST(WireParseControl, ListBackupsPageWithCursor) {
    const std::string body = R"({"BackupSummaries":[
        {"TableName":"t","BackupName":"b1","BackupStatus":"AVAILABLE","BackupArn":"arn:b1",
         "BackupCreationDateTime":1700000000,"BackupExpiryDateTime":1700000000}],
        "LastEvaluatedBackupArn":"arn:cursor"})";
    auto out = ddbsh::wire::parse_list_backups(body);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->summaries.size(), 1u);
    EXPECT_EQ(out->summaries[0].table_name, "t");
    EXPECT_EQ(out->summaries[0].backup_name, "b1");
    EXPECT_EQ(out->summaries[0].backup_status, "AVAILABLE");
    EXPECT_EQ(out->summaries[0].backup_arn, "arn:b1");
    EXPECT_EQ(out->summaries[0].creation_datetime, iso());
    EXPECT_EQ(out->summaries[0].expiry_datetime, iso());
    ASSERT_TRUE(out->last_evaluated_backup_arn.has_value());
    EXPECT_EQ(*out->last_evaluated_backup_arn, "arn:cursor");
}

TEST(WireParseControl, ListBackupsFinalPageNoCursor) {
    auto out = ddbsh::wire::parse_list_backups(R"({"BackupSummaries":[]})");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->summaries.empty());
    EXPECT_FALSE(out->last_evaluated_backup_arn.has_value());
}

// ---- ListTables ------------------------------------------------------------

TEST(WireParseControl, ListTablesPageWithCursor) {
    auto out = ddbsh::wire::parse_list_tables(
        R"({"TableNames":["a","b","c"],"LastEvaluatedTableName":"c"})");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->table_names, (std::vector<std::string>{"a", "b", "c"}));
    ASSERT_TRUE(out->last_evaluated_table_name.has_value());
    EXPECT_EQ(*out->last_evaluated_table_name, "c");
}

TEST(WireParseControl, ListTablesFinalPageNoCursor) {
    auto out = ddbsh::wire::parse_list_tables(R"({"TableNames":["only"]})");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->table_names, (std::vector<std::string>{"only"}));
    EXPECT_FALSE(out->last_evaluated_table_name.has_value());
}

// ---- SearchVectors ---------------------------------------------------------

TEST(WireParseControl, SearchVectorsResultsAndCapacity) {
    const std::string body = R"({"SearchResults":[
        {"Item":{"id":{"S":"k1"}},"Score":0.97},
        {"Item":{"id":{"S":"k2"}},"Score":0.80}],
        "ConsumedCapacity":{"VectorSearchRequestBytes":128,"VectorWriteRequestBytes":0}})";
    auto out = ddbsh::wire::parse_search_vectors(body);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->results.size(), 2u);
    EXPECT_EQ(out->results[0].item.attributes.size(), 1u);
    EXPECT_DOUBLE_EQ(out->results[0].score, 0.97);
    EXPECT_DOUBLE_EQ(out->results[1].score, 0.80);
    ASSERT_TRUE(out->vector_capacity.has_value());
    ASSERT_TRUE(out->vector_capacity->vector_search_request_bytes.has_value());
    EXPECT_DOUBLE_EQ(*out->vector_capacity->vector_search_request_bytes, 128.0);
    ASSERT_TRUE(out->vector_capacity->vector_write_request_bytes.has_value());
    EXPECT_DOUBLE_EQ(*out->vector_capacity->vector_write_request_bytes, 0.0);
}

TEST(WireParseControl, SearchVectorsNoConsumedCapacityStillSetsVectorCapacity) {
    // The adapter reads the two doubles unconditionally (default 0), so
    // vector_capacity is always set.
    auto out = ddbsh::wire::parse_search_vectors(R"({"SearchResults":[]})");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->results.empty());
    ASSERT_TRUE(out->vector_capacity.has_value());
    EXPECT_DOUBLE_EQ(*out->vector_capacity->vector_search_request_bytes, 0.0);
    EXPECT_DOUBLE_EQ(*out->vector_capacity->vector_write_request_bytes, 0.0);
}

// ---- TransactGetItems ------------------------------------------------------

TEST(WireParseControl, TransactGetItemsPresentAndEmptyResponses) {
    const std::string body = R"({"Responses":[
        {"Item":{"id":{"S":"k1"},"v":{"N":"3"}}},
        {"Item":{}},
        {}]})";
    auto out = ddbsh::wire::parse_transact_get_items(body);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->responses.size(), 3u);
    ASSERT_TRUE(out->responses[0].has_value());
    EXPECT_EQ(out->responses[0]->attributes.size(), 2u);
    EXPECT_FALSE(out->responses[1].has_value());  // empty Item -> nullopt
    EXPECT_FALSE(out->responses[2].has_value());  // no Item -> nullopt
}

// ---- token-only acks -------------------------------------------------------

TEST(WireParseControl, AckSuccessIgnoresBody) {
    // CreateTable's success body carries a TableDescription the engine ignores.
    auto out =
        ddbsh::wire::parse_create_table(R"({"TableDescription":{"TableStatus":"CREATING"}})");
    EXPECT_TRUE(out.has_value());

    EXPECT_TRUE(ddbsh::wire::parse_delete_table(R"({})").has_value());
    EXPECT_TRUE(ddbsh::wire::parse_update_table(R"({})").has_value());
    EXPECT_TRUE(ddbsh::wire::parse_create_backup(R"({})").has_value());
    EXPECT_TRUE(ddbsh::wire::parse_delete_backup(R"({})").has_value());
    EXPECT_TRUE(ddbsh::wire::parse_restore_from_backup(R"({})").has_value());
    EXPECT_TRUE(ddbsh::wire::parse_restore_to_pitr(R"({})").has_value());
    EXPECT_TRUE(ddbsh::wire::parse_transact_write_items(R"({})").has_value());
    EXPECT_TRUE(ddbsh::wire::parse_update_time_to_live(R"({})").has_value());
    EXPECT_TRUE(ddbsh::wire::parse_update_continuous_backups(R"({})").has_value());
}

TEST(WireParseControl, AckErrorDocumentMapped) {
    auto out = ddbsh::wire::parse_create_table(
        R"({"__type":"x#ResourceInUseException","message":"Table already exists: t"})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    ASSERT_TRUE(out.error().service_error_name.has_value());
    EXPECT_EQ(*out.error().service_error_name, "ResourceInUseException");
    EXPECT_EQ(out.error().message, "Table already exists: t");
}

TEST(WireParseControl, AckConditionalCheckFailedMapped) {
    // A ConditionalCheckFailedException maps to conditional_check_failed even on
    // the ack path (error_from_type_document is shared).
    auto out = ddbsh::wire::parse_transact_write_items(
        R"({"__type":"x#ConditionalCheckFailedException","message":"The conditional request failed"})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::conditional_check_failed);
}

TEST(WireParseControl, MalformedBodyIsServiceError) {
    auto out = ddbsh::wire::parse_describe_limits("not json");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
}

}  // namespace
