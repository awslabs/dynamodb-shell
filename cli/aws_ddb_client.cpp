// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_ddb_client.cpp — CLI-side IDDBClient adapter.
//
// Constructs a real DynamoDBClient with the standard credential resolution and
// optional endpoint override (mirrors the original CDDBShDDBClientConfig, arch
// §3/§10). The control-plane ops (create_table / delete_table / describe_table /
// list_tables) and the ACTIVE/gone waiters are fully implemented here. The
// data-plane ops (get_item / query / scan / put_item) still return a clear
// service_error placeholder: their ddbsh::Value <-> Aws::AttributeValue
// translation lands with the W5 data-plane round (so engine-driven goldens can
// run); the engine contract for those ops is covered offline by FakeDDBClient.

#include "aws_ddb_client.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <aws/core/client/ClientConfiguration.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/DeleteTableRequest.h>
#include <aws/dynamodb/model/DescribeContinuousBackupsRequest.h>
#include <aws/dynamodb/model/DescribeTableReplicaAutoScalingRequest.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <aws/dynamodb/model/DescribeTimeToLiveRequest.h>
#include <aws/dynamodb/model/ListTablesRequest.h>

namespace ddbsh::cli {

namespace {

// The data-plane ops have no request/response translation yet; they return
// this clear error until the W5 data-plane round. No in-scope command reaches
// them through this adapter before then (engine-driven goldens are gated on
// that round).
Error not_yet() {
    return Error{Errc::service_error, "IDDBClient data-plane operation not yet implemented",
                 std::nullopt, std::nullopt};
}

}  // namespace

AwsDDBClient::AwsDDBClient(const std::string& region, const std::string& endpoint) {
    Aws::Client::ClientConfiguration cfg;
    if (!region.empty()) {
        cfg.region = region;
    }
    if (!endpoint.empty()) {
        cfg.endpointOverride = endpoint;
    }
    client_ = std::make_unique<Aws::DynamoDB::DynamoDBClient>(cfg);
}

AwsDDBClient::~AwsDDBClient() = default;

Result<GetItemOut> AwsDDBClient::get_item(const GetItemIn&) {
    return fail(not_yet());
}

Result<QueryOut> AwsDDBClient::query(const QueryIn&) {
    return fail(not_yet());
}

Result<ScanOut> AwsDDBClient::scan(const ScanIn&) {
    return fail(not_yet());
}

Result<PutItemOut> AwsDDBClient::put_item(const PutItemIn&) {
    return fail(not_yet());
}

namespace {

// ---- CreateTable translation (ddbsh::CreateTableIn -> SDK request) ---------
// Mirrors how the original built the Aws CreateTable request directly in the
// grammar + create_table.cpp (arch §7/§8).

Aws::DynamoDB::Model::ScalarAttributeType to_sdk(ScalarType t) {
    switch (t) {
        case ScalarType::n:
            return Aws::DynamoDB::Model::ScalarAttributeType::N;
        case ScalarType::b:
            return Aws::DynamoDB::Model::ScalarAttributeType::B;
        case ScalarType::s:
            break;
    }
    return Aws::DynamoDB::Model::ScalarAttributeType::S;
}

// Build the [HASH, (RANGE)] KeySchemaElement list (the canonical order the
// original always emitted).
Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> to_sdk_key_schema(const KeySchema& ks) {
    Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> out;
    Aws::DynamoDB::Model::KeySchemaElement hash;
    hash.SetAttributeName(ks.hash);
    hash.SetKeyType(Aws::DynamoDB::Model::KeyType::HASH);
    out.push_back(hash);
    if (ks.range.has_value()) {
        Aws::DynamoDB::Model::KeySchemaElement range;
        range.SetAttributeName(*ks.range);
        range.SetKeyType(Aws::DynamoDB::Model::KeyType::RANGE);
        out.push_back(range);
    }
    return out;
}

Aws::DynamoDB::Model::Projection to_sdk_projection(const Projection& p) {
    Aws::DynamoDB::Model::Projection out;
    switch (p.type) {
        case ProjectionType::all:
            out.SetProjectionType(Aws::DynamoDB::Model::ProjectionType::ALL);
            break;
        case ProjectionType::keys_only:
            out.SetProjectionType(Aws::DynamoDB::Model::ProjectionType::KEYS_ONLY);
            break;
        case ProjectionType::include:
            out.SetProjectionType(Aws::DynamoDB::Model::ProjectionType::INCLUDE);
            for (const auto& attr : p.non_key_attributes) {
                out.AddNonKeyAttributes(attr);
            }
            break;
    }
    return out;
}

Aws::DynamoDB::Model::WarmThroughput to_sdk_warm(const WarmThroughput& w) {
    Aws::DynamoDB::Model::WarmThroughput out;
    out.SetReadUnitsPerSecond(w.read_units_per_second);
    out.SetWriteUnitsPerSecond(w.write_units_per_second);
    return out;
}

// Apply a billing spec to the CreateTableRequest. A nullopt billing defaults to
// PAY_PER_REQUEST (the original's "no billing clause" default).
void apply_billing(Aws::DynamoDB::Model::CreateTableRequest& request,
                   const std::optional<BillingModeAndThroughput>& billing) {
    if (!billing.has_value()) {
        request.SetBillingMode(Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST);
        return;
    }
    if (billing->mode == BillingMode::provisioned) {
        request.SetBillingMode(Aws::DynamoDB::Model::BillingMode::PROVISIONED);
        Aws::DynamoDB::Model::ProvisionedThroughput pt;
        pt.SetReadCapacityUnits(billing->rcu);
        pt.SetWriteCapacityUnits(billing->wcu);
        request.SetProvisionedThroughput(pt);
    } else {
        request.SetBillingMode(Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST);
        Aws::DynamoDB::Model::OnDemandThroughput od;
        if (billing->od.max_read_request_units.has_value()) {
            od.SetMaxReadRequestUnits(*billing->od.max_read_request_units);
        }
        if (billing->od.max_write_request_units.has_value()) {
            od.SetMaxWriteRequestUnits(*billing->od.max_write_request_units);
        }
        request.SetOnDemandThroughput(od);
    }
}

// A GSI has no billing MODE of its own (that is a table-level property); it
// carries only the throughput that matches the table's mode. The original's
// gsi production set ProvisionedThroughput / OnDemandThroughput directly.
void apply_gsi_throughput(Aws::DynamoDB::Model::GlobalSecondaryIndex& gsi,
                          const BillingModeAndThroughput& billing) {
    if (billing.mode == BillingMode::provisioned) {
        Aws::DynamoDB::Model::ProvisionedThroughput pt;
        pt.SetReadCapacityUnits(billing.rcu);
        pt.SetWriteCapacityUnits(billing.wcu);
        gsi.SetProvisionedThroughput(pt);
    } else {
        Aws::DynamoDB::Model::OnDemandThroughput od;
        if (billing.od.max_read_request_units.has_value()) {
            od.SetMaxReadRequestUnits(*billing.od.max_read_request_units);
        }
        if (billing.od.max_write_request_units.has_value()) {
            od.SetMaxWriteRequestUnits(*billing.od.max_write_request_units);
        }
        gsi.SetOnDemandThroughput(od);
    }
}

}  // namespace

Result<CreateTableOut> AwsDDBClient::create_table(const CreateTableIn& in) {
    Aws::DynamoDB::Model::CreateTableRequest request;
    request.SetTableName(in.table_name);

    for (const auto& attr : in.attribute_definitions) {
        Aws::DynamoDB::Model::AttributeDefinition def;
        def.SetAttributeName(attr.name);
        def.SetAttributeType(to_sdk(attr.type));
        request.AddAttributeDefinitions(def);
    }

    request.SetKeySchema(to_sdk_key_schema(in.key_schema));

    apply_billing(request, in.billing);

    if (in.warm.has_value()) {
        request.SetWarmThroughput(to_sdk_warm(*in.warm));
    }

    for (const auto& gsi : in.gsis) {
        Aws::DynamoDB::Model::GlobalSecondaryIndex sdk_gsi;
        sdk_gsi.SetIndexName(gsi.name);
        sdk_gsi.SetKeySchema(to_sdk_key_schema(gsi.key_schema));
        sdk_gsi.SetProjection(to_sdk_projection(gsi.projection));
        if (gsi.billing.has_value()) {
            apply_gsi_throughput(sdk_gsi, *gsi.billing);
        }
        if (gsi.warm.has_value()) {
            sdk_gsi.SetWarmThroughput(to_sdk_warm(*gsi.warm));
        }
        request.AddGlobalSecondaryIndexes(sdk_gsi);
    }

    for (const auto& lsi : in.lsis) {
        Aws::DynamoDB::Model::LocalSecondaryIndex sdk_lsi;
        sdk_lsi.SetIndexName(lsi.name);
        sdk_lsi.SetKeySchema(to_sdk_key_schema(lsi.key_schema));
        sdk_lsi.SetProjection(to_sdk_projection(lsi.projection));
        request.AddLocalSecondaryIndexes(sdk_lsi);
    }

    if (in.stream.has_value()) {
        Aws::DynamoDB::Model::StreamSpecification stream;
        stream.SetStreamEnabled(in.stream->enabled);
        if (in.stream->enabled) {
            switch (in.stream->view_type) {
                case StreamViewType::keys_only:
                    stream.SetStreamViewType(Aws::DynamoDB::Model::StreamViewType::KEYS_ONLY);
                    break;
                case StreamViewType::new_image:
                    stream.SetStreamViewType(Aws::DynamoDB::Model::StreamViewType::NEW_IMAGE);
                    break;
                case StreamViewType::old_image:
                    stream.SetStreamViewType(Aws::DynamoDB::Model::StreamViewType::OLD_IMAGE);
                    break;
                case StreamViewType::new_and_old_images:
                    stream.SetStreamViewType(
                        Aws::DynamoDB::Model::StreamViewType::NEW_AND_OLD_IMAGES);
                    break;
            }
        }
        request.SetStreamSpecification(stream);
    }

    if (in.sse.has_value()) {
        Aws::DynamoDB::Model::SSESpecification sse;
        sse.SetEnabled(in.sse->enabled);
        if (in.sse->enabled) {
            if (in.sse->sse_type == SseType::aes256) {
                sse.SetSSEType(Aws::DynamoDB::Model::SSEType::AES256);
            } else {
                sse.SetSSEType(Aws::DynamoDB::Model::SSEType::KMS);
            }
            if (in.sse->kms_master_key_id.has_value()) {
                sse.SetKMSMasterKeyId(*in.sse->kms_master_key_id);
            }
        }
        request.SetSSESpecification(sse);
    }

    request.SetTableClass(in.table_class == TableClass::standard_infrequent_access
                              ? Aws::DynamoDB::Model::TableClass::STANDARD_INFREQUENT_ACCESS
                              : Aws::DynamoDB::Model::TableClass::STANDARD);

    request.SetDeletionProtectionEnabled(in.deletion_protection);

    for (const auto& tag : in.tags) {
        Aws::DynamoDB::Model::Tag sdk_tag;
        sdk_tag.SetKey(tag.key);
        sdk_tag.SetValue(tag.value);
        request.AddTags(sdk_tag);
    }

    auto outcome = client_->CreateTable(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return CreateTableOut{};
}

Result<DeleteTableOut> AwsDDBClient::delete_table(const DeleteTableIn& in) {
    Aws::DynamoDB::Model::DeleteTableRequest request;
    request.SetTableName(in.table_name);
    auto outcome = client_->DeleteTable(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return DeleteTableOut{};
}

Result<ListTablesOut> AwsDDBClient::list_tables(const ListTablesIn&) {
    // Follow LastEvaluatedTableName so the caller gets the complete list even
    // when the account has more tables than one page returns.
    ListTablesOut out;
    Aws::String start_table;
    do {
        Aws::DynamoDB::Model::ListTablesRequest request;
        if (!start_table.empty()) {
            request.SetExclusiveStartTableName(start_table);
        }
        auto outcome = client_->ListTables(request);
        if (!outcome.IsSuccess()) {
            const auto& err = outcome.GetError();
            return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                              std::string(err.GetExceptionName().c_str()),
                              std::string(err.GetRequestId().c_str())});
        }
        const auto& result = outcome.GetResult();
        for (const auto& name : result.GetTableNames()) {
            out.table_names.emplace_back(name.c_str());
        }
        start_table = result.GetLastEvaluatedTableName();
    } while (!start_table.empty());
    return out;
}

namespace {

// Load a HASH/RANGE key schema from an SDK KeySchemaElement list into the
// SDK-free TableKeySchema (ignores any future key types, as the planner only
// needs pk/rk; the original logged-and-failed on unknown types).
TableKeySchema load_key_schema(
    const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement>& key_schema) {
    TableKeySchema out;
    for (const auto& element : key_schema) {
        if (element.GetKeyType() == Aws::DynamoDB::Model::KeyType::HASH) {
            out.hash_key = element.GetAttributeName().c_str();
        } else if (element.GetKeyType() == Aws::DynamoDB::Model::KeyType::RANGE) {
            out.range_key = element.GetAttributeName().c_str();
        }
    }
    return out;
}

// ---- SDK -> library DESCRIBE-detail translation ----------------------------

ScalarType from_sdk_scalar(Aws::DynamoDB::Model::ScalarAttributeType t) {
    switch (t) {
        case Aws::DynamoDB::Model::ScalarAttributeType::N:
            return ScalarType::n;
        case Aws::DynamoDB::Model::ScalarAttributeType::B:
            return ScalarType::b;
        default:
            return ScalarType::s;
    }
}

Projection from_sdk_projection(const Aws::DynamoDB::Model::Projection& p) {
    Projection out;
    switch (p.GetProjectionType()) {
        case Aws::DynamoDB::Model::ProjectionType::KEYS_ONLY:
            out.type = ProjectionType::keys_only;
            break;
        case Aws::DynamoDB::Model::ProjectionType::INCLUDE:
            out.type = ProjectionType::include;
            for (const auto& a : p.GetNonKeyAttributes()) {
                out.non_key_attributes.emplace_back(a.c_str());
            }
            break;
        default:
            out.type = ProjectionType::all;
            break;
    }
    return out;
}

// Load the full DESCRIBE detail for one GSI (key schema + projection, status,
// backfilling, both throughput shapes, warm throughput). The DESCRIBE executor
// chooses provisioned-vs-on-demand display from the table's billing mode, so
// both throughput shapes are loaded (matching describe.cpp __show_gsi).
SecondaryIndexSchema load_gsi_detail(
    const Aws::DynamoDB::Model::GlobalSecondaryIndexDescription& gsi) {
    SecondaryIndexSchema out;
    out.index_name = gsi.GetIndexName().c_str();
    out.key_schema = load_key_schema(gsi.GetKeySchema());
    if (gsi.ProjectionHasBeenSet()) {
        out.projection = from_sdk_projection(gsi.GetProjection());
        out.projection_set = true;
    }
    out.index_status =
        Aws::DynamoDB::Model::IndexStatusMapper::GetNameForIndexStatus(gsi.GetIndexStatus())
            .c_str();
    out.backfilling = gsi.GetBackfilling();
    const auto& pt = gsi.GetProvisionedThroughput();
    out.provisioned_read_capacity_units = pt.GetReadCapacityUnits();
    out.provisioned_write_capacity_units = pt.GetWriteCapacityUnits();
    if (gsi.OnDemandThroughputHasBeenSet()) {
        out.on_demand_throughput_set = true;
        const auto& od = gsi.GetOnDemandThroughput();
        out.on_demand_max_read_request_units =
            od.MaxReadRequestUnitsHasBeenSet() ? od.GetMaxReadRequestUnits() : -1;
        out.on_demand_max_write_request_units =
            od.MaxWriteRequestUnitsHasBeenSet() ? od.GetMaxWriteRequestUnits() : -1;
    }
    if (gsi.WarmThroughputHasBeenSet()) {
        const auto& w = gsi.GetWarmThroughput();
        out.warm_throughput = WarmThroughputDescription{
            w.GetReadUnitsPerSecond(), w.GetWriteUnitsPerSecond(),
            Aws::DynamoDB::Model::IndexStatusMapper::GetNameForIndexStatus(w.GetStatus()).c_str()};
    }
    return out;
}

}  // namespace

Result<DescribeTableOut> AwsDDBClient::describe_table(const DescribeTableIn& in) {
    Aws::DynamoDB::Model::DescribeTableRequest request;
    request.SetTableName(in.table_name);
    auto outcome = client_->DescribeTable(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }

    const auto& table = outcome.GetResult().GetTable();
    DescribeTableOut out;
    out.table_name = table.GetTableName().c_str();
    out.key_schema = load_key_schema(table.GetKeySchema());
    for (const auto& lsi : table.GetLocalSecondaryIndexes()) {
        out.local_secondary_indexes.push_back(
            SecondaryIndexSchema{.index_name = lsi.GetIndexName().c_str(),
                                 .key_schema = load_key_schema(lsi.GetKeySchema())});
    }
    for (const auto& gsi : table.GetGlobalSecondaryIndexes()) {
        out.global_secondary_indexes.push_back(load_gsi_detail(gsi));
    }

    // Report fields (SHOW TABLES / DESCRIBE). Enum values are stored as the SDK
    // mapper's name string; the `has`-style optionals mirror the SDK's
    // *HasBeenSet() so the report reproduces the original's defaulting markers.
    out.table_status =
        Aws::DynamoDB::Model::TableStatusMapper::GetNameForTableStatus(table.GetTableStatus())
            .c_str();
    out.table_id = table.GetTableId().c_str();
    out.table_arn = table.GetTableArn().c_str();
    if (table.BillingModeSummaryHasBeenSet()) {
        out.billing_mode_summary = Aws::DynamoDB::Model::BillingModeMapper::GetNameForBillingMode(
                                       table.GetBillingModeSummary().GetBillingMode())
                                       .c_str();
    }
    out.provisioned_throughput_set = table.ProvisionedThroughputHasBeenSet();
    if (table.TableClassSummaryHasBeenSet()) {
        out.table_class_summary = Aws::DynamoDB::Model::TableClassMapper::GetNameForTableClass(
                                      table.GetTableClassSummary().GetTableClass())
                                      .c_str();
    }

    // Full DESCRIBE report fields (describe.cpp). SHOW TABLES ignores these.
    for (const auto& attr : table.GetAttributeDefinitions()) {
        out.attribute_definitions.push_back(AttributeDef{attr.GetAttributeName().c_str(),
                                                         from_sdk_scalar(attr.GetAttributeType())});
    }
    out.creation_date_time =
        table.GetCreationDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str();
    out.table_size_bytes = table.GetTableSizeBytes();
    out.item_count = table.GetItemCount();
    out.deletion_protection_enabled = table.GetDeletionProtectionEnabled();
    {
        const auto& pt = table.GetProvisionedThroughput();
        out.provisioned_read_capacity_units = pt.GetReadCapacityUnits();
        out.provisioned_write_capacity_units = pt.GetWriteCapacityUnits();
    }
    if (table.OnDemandThroughputHasBeenSet()) {
        out.on_demand_throughput_set = true;
        const auto& od = table.GetOnDemandThroughput();
        out.on_demand_max_read_request_units =
            od.MaxReadRequestUnitsHasBeenSet() ? od.GetMaxReadRequestUnits() : -1;
        out.on_demand_max_write_request_units =
            od.MaxWriteRequestUnitsHasBeenSet() ? od.GetMaxWriteRequestUnits() : -1;
    }
    if (table.WarmThroughputHasBeenSet()) {
        const auto& w = table.GetWarmThroughput();
        out.warm_throughput = WarmThroughputDescription{
            w.GetReadUnitsPerSecond(), w.GetWriteUnitsPerSecond(),
            Aws::DynamoDB::Model::TableStatusMapper::GetNameForTableStatus(w.GetStatus()).c_str()};
    }
    if (table.StreamSpecificationHasBeenSet() &&
        table.GetStreamSpecification().GetStreamEnabled()) {
        out.stream_view_type = Aws::DynamoDB::Model::StreamViewTypeMapper::GetNameForStreamViewType(
                                   table.GetStreamSpecification().GetStreamViewType())
                                   .c_str();
    }
    if (table.SSEDescriptionHasBeenSet()) {
        out.sse_status = Aws::DynamoDB::Model::SSEStatusMapper::GetNameForSSEStatus(
                             table.GetSSEDescription().GetStatus())
                             .c_str();
    }
    return out;
}

Result<TimeToLiveOut> AwsDDBClient::describe_time_to_live(const DescribeTimeToLiveIn& in) {
    Aws::DynamoDB::Model::DescribeTimeToLiveRequest request;
    request.SetTableName(in.table_name);
    auto outcome = client_->DescribeTimeToLive(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    const auto& desc = outcome.GetResult().GetTimeToLiveDescription();
    TimeToLiveOut out;
    out.status = Aws::DynamoDB::Model::TimeToLiveStatusMapper::GetNameForTimeToLiveStatus(
                     desc.GetTimeToLiveStatus())
                     .c_str();
    out.attribute_name = desc.GetAttributeName().c_str();
    return out;
}

Result<ContinuousBackupsOut> AwsDDBClient::describe_continuous_backups(
    const DescribeContinuousBackupsIn& in) {
    Aws::DynamoDB::Model::DescribeContinuousBackupsRequest request;
    request.SetTableName(in.table_name);
    auto outcome = client_->DescribeContinuousBackups(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    const auto& pitr =
        outcome.GetResult().GetContinuousBackupsDescription().GetPointInTimeRecoveryDescription();
    ContinuousBackupsOut out;
    out.pitr_status =
        Aws::DynamoDB::Model::PointInTimeRecoveryStatusMapper::GetNameForPointInTimeRecoveryStatus(
            pitr.GetPointInTimeRecoveryStatus())
            .c_str();
    if (pitr.GetPointInTimeRecoveryStatus() ==
        Aws::DynamoDB::Model::PointInTimeRecoveryStatus::ENABLED) {
        out.earliest_restorable_date_time = pitr.GetEarliestRestorableDateTime()
                                                .ToGmtString(Aws::Utils::DateFormat::ISO_8601)
                                                .c_str();
        out.latest_restorable_date_time = pitr.GetLatestRestorableDateTime()
                                              .ToGmtString(Aws::Utils::DateFormat::ISO_8601)
                                              .c_str();
    }
    return out;
}

Result<TableReplicaAutoScalingOut> AwsDDBClient::describe_table_replica_auto_scaling(
    const DescribeTableReplicaAutoScalingIn& in) {
    Aws::DynamoDB::Model::DescribeTableReplicaAutoScalingRequest request;
    request.SetTableName(in.table_name);
    auto outcome = client_->DescribeTableReplicaAutoScaling(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    TableReplicaAutoScalingOut out;
    const auto& asd = outcome.GetResult().GetTableAutoScalingDescription();
    for (const auto& repl : asd.GetReplicas()) {
        out.replicas.push_back(ReplicaAutoScalingDescription{
            repl.GetRegionName().c_str(),
            Aws::DynamoDB::Model::ReplicaStatusMapper::GetNameForReplicaStatus(
                repl.GetReplicaStatus())
                .c_str()});
    }
    return out;
}

Result<void> AwsDDBClient::wait_for_table_gone(const std::string& table_name) {
    // Bounded exponential backoff: 100ms doubling to a 2s cap (arch §10). Poll
    // DescribeTable until the table reports RESOURCE_NOT_FOUND (gone). A
    // non-retryable error is terminal and returned; a retryable/transient error
    // (throttling, InternalServerError) is treated like "still transitioning"
    // and polled again, matching the original wait_for_table_gone.
    std::chrono::milliseconds interval(100);
    const std::chrono::milliseconds cap(2000);
    while (true) {
        Aws::DynamoDB::Model::DescribeTableRequest request;
        request.SetTableName(table_name);
        auto outcome = client_->DescribeTable(request);
        if (!outcome.IsSuccess()) {
            const auto& err = outcome.GetError();
            if (err.GetErrorType() == Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND) {
                return {};  // gone
            }
            if (!err.ShouldRetry()) {
                return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                                  std::string(err.GetExceptionName().c_str()),
                                  std::string(err.GetRequestId().c_str())});
            }
            // transient: fall through and keep polling
        }
        std::this_thread::sleep_for(interval);
        interval = std::min(interval * 2, cap);
    }
}

Result<void> AwsDDBClient::wait_for_table_active(const std::string& table_name) {
    // Bounded exponential backoff: 100ms doubling to a 2s cap (arch §10). Poll
    // DescribeTable until the table reports ACTIVE. CREATING/UPDATING keep
    // polling; any other status cannot progress to ACTIVE and is terminal.
    // RESOURCE_NOT_FOUND right after CreateTable is eventual-consistency and is
    // polled again; a non-retryable error is terminal; a transient error keeps
    // polling (port of the original wait_for_table_active).
    using Status = Aws::DynamoDB::Model::TableStatus;
    std::chrono::milliseconds interval(100);
    const std::chrono::milliseconds cap(2000);
    while (true) {
        Aws::DynamoDB::Model::DescribeTableRequest request;
        request.SetTableName(table_name);
        auto outcome = client_->DescribeTable(request);
        if (outcome.IsSuccess()) {
            const Status status = outcome.GetResult().GetTable().GetTableStatus();
            if (status == Status::ACTIVE) {
                return {};
            }
            if (status != Status::CREATING && status != Status::UPDATING) {
                return fail(
                    Error{Errc::service_error,
                          "Table " + table_name + " is in state " +
                              Aws::DynamoDB::Model::TableStatusMapper::GetNameForTableStatus(status)
                                  .c_str() +
                              " and will not become ACTIVE.",
                          std::nullopt, std::nullopt});
            }
            // CREATING/UPDATING: keep polling.
        } else {
            const auto& err = outcome.GetError();
            if (err.GetErrorType() != Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND &&
                !err.ShouldRetry()) {
                return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                                  std::string(err.GetExceptionName().c_str()),
                                  std::string(err.GetRequestId().c_str())});
            }
            // not-found (eventual consistency) or transient: keep polling.
        }
        std::this_thread::sleep_for(interval);
        interval = std::min(interval * 2, cap);
    }
}

}  // namespace ddbsh::cli
