// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_ddb_client.cpp — CLI-side IDDBClient adapter.
//
// Constructs a real DynamoDBClient with the standard credential resolution and
// optional endpoint override (mirrors the original CDDBShDDBClientConfig, arch
// §3/§10). All in-scope ops are now implemented here: the control-plane ops
// (create_table / delete_table / describe_table / describe_time_to_live /
// describe_continuous_backups / describe_table_replica_auto_scaling /
// list_tables), the ACTIVE/gone waiters, and the data-plane ops (get_item /
// query / scan / put_item) with full ddbsh::Value <-> Aws::AttributeValue
// translation. This is the sole place the SDK meets the library's SDK-free
// structs (D11).

#include "aws_ddb_client.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <type_traits>

#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/dynamodb/model/AttributeValue.h>
#include <aws/dynamodb/model/BackupStatus.h>
#include <aws/dynamodb/model/BackupType.h>
#include <aws/dynamodb/model/BackupTypeFilter.h>
#include <aws/dynamodb/model/CreateBackupRequest.h>
#include <aws/dynamodb/model/CreateGlobalSecondaryIndexAction.h>
#include <aws/dynamodb/model/CreateReplicationGroupMemberAction.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/CreateVectorIndexAction.h>
#include <aws/dynamodb/model/DeleteBackupRequest.h>
#include <aws/dynamodb/model/DeleteGlobalSecondaryIndexAction.h>
#include <aws/dynamodb/model/DeleteItemRequest.h>
#include <aws/dynamodb/model/DeleteReplicationGroupMemberAction.h>
#include <aws/dynamodb/model/DeleteTableRequest.h>
#include <aws/dynamodb/model/DeleteVectorIndexAction.h>
#include <aws/dynamodb/model/DescribeBackupRequest.h>
#include <aws/dynamodb/model/DescribeContinuousBackupsRequest.h>
#include <aws/dynamodb/model/DescribeLimitsRequest.h>
#include <aws/dynamodb/model/DescribeTableReplicaAutoScalingRequest.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <aws/dynamodb/model/DescribeTimeToLiveRequest.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/GlobalSecondaryIndexUpdate.h>
#include <aws/dynamodb/model/ListBackupsRequest.h>
#include <aws/dynamodb/model/ListTablesRequest.h>
#include <aws/dynamodb/model/ProvisionedThroughputOverride.h>
#include <aws/dynamodb/model/PutItemRequest.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/ReplicaGlobalSecondaryIndex.h>
#include <aws/dynamodb/model/ReplicationGroupUpdate.h>
#include <aws/dynamodb/model/RestoreTableFromBackupRequest.h>
#include <aws/dynamodb/model/RestoreTableToPointInTimeRequest.h>
#include <aws/dynamodb/model/ScanRequest.h>
#include <aws/dynamodb/model/SearchSchemaElement.h>
#include <aws/dynamodb/model/SearchVectorsRequest.h>
#include <aws/dynamodb/model/TimeToLiveSpecification.h>
#include <aws/dynamodb/model/TransactGetItemsRequest.h>
#include <aws/dynamodb/model/TransactWriteItemsRequest.h>
#include <aws/dynamodb/model/UpdateContinuousBackupsRequest.h>
#include <aws/dynamodb/model/UpdateGlobalSecondaryIndexAction.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>
#include <aws/dynamodb/model/UpdateReplicationGroupMemberAction.h>
#include <aws/dynamodb/model/UpdateTableRequest.h>
#include <aws/dynamodb/model/UpdateTimeToLiveRequest.h>
#include <aws/dynamodb/model/VectorAttributeDefinition.h>
#include <aws/dynamodb/model/VectorIndex.h>
#include <aws/dynamodb/model/VectorIndexUpdate.h>
namespace ddbsh::cli {

namespace {

using SdkAttributeValue = Aws::DynamoDB::Model::AttributeValue;
using SdkValueType = Aws::DynamoDB::Model::ValueType;

// Turn a service error into the library Error (byte-exact message carried
// through to the Renderer; a ConditionalCheckFailedException's message is the
// service's own "The conditional request failed", arch §9). A
// ConditionalCheckFailed is tagged with Errc::conditional_check_failed so the
// UPDATE/DELETE executors can intercept it and count it as a ccf (D-2c-7)
// rather than aborting; INSERT/PutItem still surfaces it as a hard error (the
// executor does not intercept it there), preserving the original's behavior.
Error from_sdk_error(const Aws::Client::AWSError<Aws::DynamoDB::DynamoDBErrors>& err) {
    const Errc code = err.GetErrorType() == Aws::DynamoDB::DynamoDBErrors::CONDITIONAL_CHECK_FAILED
                          ? Errc::conditional_check_failed
                          : Errc::service_error;
    return Error{code, err.GetMessage().c_str(), std::string(err.GetExceptionName().c_str()),
                 std::string(err.GetRequestId().c_str())};
}

// ---- ddbsh::Value <-> Aws::DynamoDB::Model::AttributeValue (D11 seam) ------
// This is the SOLE place the library's SDK-free Value meets the SDK's recursive
// AttributeValue. The parser only ever builds S/N/BOOL/NUL/L/M literals (arch
// §4 — no binary/set input), but responses can carry any type, so both
// directions handle all ten. Empty L/M are marked explicitly (SetL/SetM with an
// empty collection) so an empty `[]`/`{}` round-trips as a list/map, not an
// unset value (arch §9 supports empty collections).

SdkAttributeValue to_sdk_value(const Value& v);

Aws::Map<Aws::String, SdkAttributeValue> to_sdk_item(const ValueMap& item) {
    Aws::Map<Aws::String, SdkAttributeValue> out;
    for (const auto& [name, value] : item) {
        out.emplace(Aws::String(name.c_str()), to_sdk_value(value));
    }
    return out;
}

SdkAttributeValue to_sdk_value(const Value& v) {
    SdkAttributeValue out;
    switch (v.type) {
        case Value::Type::S:
            out.SetS(Aws::String(v.s.c_str()));
            break;
        case Value::Type::N:
            out.SetN(Aws::String(v.s.c_str()));
            break;
        case Value::Type::B:
            out.SetB(Aws::Utils::HashingUtils::Base64Decode(Aws::String(v.s.c_str())));
            break;
        case Value::Type::BOOL:
            out.SetBool(v.boolean);
            break;
        case Value::Type::NUL:
            out.SetNull(true);
            break;
        case Value::Type::L: {
            Aws::Vector<std::shared_ptr<SdkAttributeValue>> items;
            items.reserve(v.list.size());
            for (const auto& e : v.list) {
                items.push_back(std::make_shared<SdkAttributeValue>(to_sdk_value(e)));
            }
            out.SetL(items);
            break;
        }
        case Value::Type::M: {
            Aws::Map<Aws::String, const std::shared_ptr<SdkAttributeValue>> m;
            for (const auto& [k, val] : v.map) {
                m.emplace(Aws::String(k.c_str()),
                          std::make_shared<SdkAttributeValue>(to_sdk_value(val)));
            }
            out.SetM(m);
            break;
        }
        case Value::Type::SS: {
            Aws::Vector<Aws::String> ss;
            ss.reserve(v.set.size());
            for (const auto& s : v.set) {
                ss.emplace_back(s.c_str());
            }
            out.SetSS(ss);
            break;
        }
        case Value::Type::NS: {
            Aws::Vector<Aws::String> ns;
            ns.reserve(v.set.size());
            for (const auto& s : v.set) {
                ns.emplace_back(s.c_str());
            }
            out.SetNS(ns);
            break;
        }
        case Value::Type::BS: {
            Aws::Vector<Aws::Utils::ByteBuffer> bs;
            bs.reserve(v.set.size());
            for (const auto& s : v.set) {
                bs.push_back(Aws::Utils::HashingUtils::Base64Decode(Aws::String(s.c_str())));
            }
            out.SetBS(bs);
            break;
        }
    }
    return out;
}

Value from_sdk_value(const SdkAttributeValue& v);

ValueMap from_sdk_item(const Aws::Map<Aws::String, SdkAttributeValue>& item) {
    ValueMap out;
    out.reserve(item.size());
    for (const auto& [name, value] : item) {
        out.emplace_back(std::string(name.c_str()), from_sdk_value(value));
    }
    return out;
}

Value from_sdk_value(const SdkAttributeValue& v) {
    switch (v.GetType()) {
        case SdkValueType::STRING:
            return Value::string_value(v.GetS().c_str());
        case SdkValueType::NUMBER:
            return Value::number_value(v.GetN().c_str());
        case SdkValueType::BOOL:
            return Value::bool_value(v.GetBool());
        case SdkValueType::NULLVALUE:
            return Value::null_value();
        case SdkValueType::BYTEBUFFER: {
            Value out;
            out.type = Value::Type::B;
            out.s = Aws::Utils::HashingUtils::Base64Encode(v.GetB()).c_str();
            return out;
        }
        case SdkValueType::ATTRIBUTE_LIST: {
            Value out;
            out.type = Value::Type::L;
            for (const auto& e : v.GetL()) {
                out.list.push_back(from_sdk_value(*e));
            }
            return out;
        }
        case SdkValueType::ATTRIBUTE_MAP: {
            Value out;
            out.type = Value::Type::M;
            for (const auto& [k, val] : v.GetM()) {
                out.map.emplace_back(std::string(k.c_str()), from_sdk_value(*val));
            }
            return out;
        }
        case SdkValueType::STRING_SET: {
            Value out;
            out.type = Value::Type::SS;
            for (const auto& s : v.GetSS()) {
                out.set.emplace_back(s.c_str());
            }
            return out;
        }
        case SdkValueType::NUMBER_SET: {
            Value out;
            out.type = Value::Type::NS;
            for (const auto& s : v.GetNS()) {
                out.set.emplace_back(s.c_str());
            }
            return out;
        }
        case SdkValueType::BYTEBUFFER_SET: {
            Value out;
            out.type = Value::Type::BS;
            for (const auto& b : v.GetBS()) {
                out.set.emplace_back(Aws::Utils::HashingUtils::Base64Encode(b).c_str());
            }
            return out;
        }
    }
    return Value::null_value();  // unreachable; keeps the compiler happy
}

// ExpressionAttributeNames: library ordered pairs -> SDK string map.
Aws::Map<Aws::String, Aws::String> to_sdk_names(
    const std::vector<std::pair<std::string, std::string>>& names) {
    Aws::Map<Aws::String, Aws::String> out;
    for (const auto& [token, name] : names) {
        out.emplace(Aws::String(token.c_str()), Aws::String(name.c_str()));
    }
    return out;
}

Aws::DynamoDB::Model::ReturnConsumedCapacity to_sdk_rcc(ReturnConsumedCapacity rcc) {
    switch (rcc) {
        case ReturnConsumedCapacity::total:
            return Aws::DynamoDB::Model::ReturnConsumedCapacity::TOTAL;
        case ReturnConsumedCapacity::indexes:
            return Aws::DynamoDB::Model::ReturnConsumedCapacity::INDEXES;
        case ReturnConsumedCapacity::none:
            break;
    }
    return Aws::DynamoDB::Model::ReturnConsumedCapacity::NONE;
}

// Translate the SDK ConsumedCapacity into the library's optional view. The
// service only populates it when ReturnConsumedCapacity != NONE, so when no
// sub-field has been set we return nullopt (nothing to totalize/display).
std::optional<ConsumedCapacity> from_sdk_consumed_capacity(
    const Aws::DynamoDB::Model::ConsumedCapacity& cc) {
    ConsumedCapacity out;
    bool any = false;
    if (cc.TableNameHasBeenSet()) {
        out.table_name = cc.GetTableName().c_str();
        any = true;
    }
    if (cc.CapacityUnitsHasBeenSet()) {
        out.capacity_units = cc.GetCapacityUnits();
        any = true;
    }
    if (cc.ReadCapacityUnitsHasBeenSet()) {
        out.read_capacity_units = cc.GetReadCapacityUnits();
        any = true;
    }
    if (cc.WriteCapacityUnitsHasBeenSet()) {
        out.write_capacity_units = cc.GetWriteCapacityUnits();
        any = true;
    }
    if (!any) {
        return std::nullopt;
    }
    return out;
}

// ---- Request builders (shared by the execute ops and explain_request) ------
// Building the SDK request is factored out of each execute op so EXPLAIN
// serializes the EXACT request the op would send (byte-identical to what runs),
// per P2a-03 §4.3.

Aws::DynamoDB::Model::GetItemRequest build_get_item(const GetItemIn& in) {
    Aws::DynamoDB::Model::GetItemRequest request;
    request.SetTableName(in.table_name);
    request.SetKey(to_sdk_item(in.key));
    if (in.projection_expression.has_value()) {
        request.SetProjectionExpression(Aws::String(in.projection_expression->c_str()));
    }
    if (!in.expression_attribute_names.empty()) {
        request.SetExpressionAttributeNames(to_sdk_names(in.expression_attribute_names));
    }
    request.SetConsistentRead(in.consistent_read);
    request.SetReturnConsumedCapacity(to_sdk_rcc(in.return_consumed_capacity));
    return request;
}

Aws::DynamoDB::Model::QueryRequest build_query(const QueryIn& in) {
    Aws::DynamoDB::Model::QueryRequest request;
    request.SetTableName(in.table_name);
    if (in.index_name.has_value()) {
        request.SetIndexName(Aws::String(in.index_name->c_str()));
    }
    request.SetKeyConditionExpression(Aws::String(in.key_condition_expression.c_str()));
    if (in.filter_expression.has_value()) {
        request.SetFilterExpression(Aws::String(in.filter_expression->c_str()));
    }
    if (in.projection_expression.has_value()) {
        request.SetProjectionExpression(Aws::String(in.projection_expression->c_str()));
    }
    if (!in.expression_attribute_names.empty()) {
        request.SetExpressionAttributeNames(to_sdk_names(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        request.SetExpressionAttributeValues(to_sdk_item(in.expression_attribute_values));
    }
    request.SetConsistentRead(in.consistent_read);
    request.SetReturnConsumedCapacity(to_sdk_rcc(in.return_consumed_capacity));
    if (in.exclusive_start_key.has_value()) {
        request.SetExclusiveStartKey(to_sdk_item(*in.exclusive_start_key));
    }
    return request;
}

Aws::DynamoDB::Model::ScanRequest build_scan(const ScanIn& in) {
    Aws::DynamoDB::Model::ScanRequest request;
    request.SetTableName(in.table_name);
    if (in.index_name.has_value()) {
        request.SetIndexName(Aws::String(in.index_name->c_str()));
    }
    if (in.filter_expression.has_value()) {
        request.SetFilterExpression(Aws::String(in.filter_expression->c_str()));
    }
    if (in.projection_expression.has_value()) {
        request.SetProjectionExpression(Aws::String(in.projection_expression->c_str()));
    }
    if (!in.expression_attribute_names.empty()) {
        request.SetExpressionAttributeNames(to_sdk_names(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        request.SetExpressionAttributeValues(to_sdk_item(in.expression_attribute_values));
    }
    request.SetConsistentRead(in.consistent_read);
    request.SetReturnConsumedCapacity(to_sdk_rcc(in.return_consumed_capacity));
    if (in.exclusive_start_key.has_value()) {
        request.SetExclusiveStartKey(to_sdk_item(*in.exclusive_start_key));
    }
    return request;
}

// RETURN VALUES selector -> SDK enum. Threaded into PutItem (REPLACE) and
// UpdateItem/DeleteItem; the returned-item display is rendered from the *Out
// attributes (2g).
Aws::DynamoDB::Model::ReturnValue to_sdk_return_value(ReturnValue rv) {
    switch (rv) {
        case ReturnValue::all_old:
            return Aws::DynamoDB::Model::ReturnValue::ALL_OLD;
        case ReturnValue::all_new:
            return Aws::DynamoDB::Model::ReturnValue::ALL_NEW;
        case ReturnValue::updated_old:
            return Aws::DynamoDB::Model::ReturnValue::UPDATED_OLD;
        case ReturnValue::updated_new:
            return Aws::DynamoDB::Model::ReturnValue::UPDATED_NEW;
        case ReturnValue::none:
            break;
    }
    return Aws::DynamoDB::Model::ReturnValue::NONE;
}

Aws::DynamoDB::Model::PutItemRequest build_put_item(const PutItemIn& in) {
    Aws::DynamoDB::Model::PutItemRequest request;
    request.SetTableName(in.table_name);
    request.SetItem(to_sdk_item(in.item));
    if (in.condition_expression.has_value()) {
        request.SetConditionExpression(Aws::String(in.condition_expression->c_str()));
    }
    if (!in.expression_attribute_names.empty()) {
        request.SetExpressionAttributeNames(to_sdk_names(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        request.SetExpressionAttributeValues(to_sdk_item(in.expression_attribute_values));
    }
    if (in.return_values != ReturnValue::none) {
        request.SetReturnValues(to_sdk_return_value(in.return_values));
    }
    if (in.return_consumed_capacity != ReturnConsumedCapacity::none) {
        request.SetReturnConsumedCapacity(to_sdk_rcc(in.return_consumed_capacity));
    }
    return request;
}

Aws::DynamoDB::Model::UpdateItemRequest build_update_item(const UpdateItemIn& in) {
    Aws::DynamoDB::Model::UpdateItemRequest request;
    request.SetTableName(in.table_name);
    request.SetKey(to_sdk_item(in.key));
    request.SetUpdateExpression(Aws::String(in.update_expression.c_str()));
    if (in.condition_expression.has_value()) {
        request.SetConditionExpression(Aws::String(in.condition_expression->c_str()));
    }
    if (!in.expression_attribute_names.empty()) {
        request.SetExpressionAttributeNames(to_sdk_names(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        request.SetExpressionAttributeValues(to_sdk_item(in.expression_attribute_values));
    }
    if (in.return_values != ReturnValue::none) {
        request.SetReturnValues(to_sdk_return_value(in.return_values));
    }
    if (in.return_consumed_capacity != ReturnConsumedCapacity::none) {
        request.SetReturnConsumedCapacity(to_sdk_rcc(in.return_consumed_capacity));
    }
    return request;
}

Aws::DynamoDB::Model::DeleteItemRequest build_delete_item(const DeleteItemIn& in) {
    Aws::DynamoDB::Model::DeleteItemRequest request;
    request.SetTableName(in.table_name);
    request.SetKey(to_sdk_item(in.key));
    if (in.condition_expression.has_value()) {
        request.SetConditionExpression(Aws::String(in.condition_expression->c_str()));
    }
    if (!in.expression_attribute_names.empty()) {
        request.SetExpressionAttributeNames(to_sdk_names(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        request.SetExpressionAttributeValues(to_sdk_item(in.expression_attribute_values));
    }
    if (in.return_values != ReturnValue::none) {
        request.SetReturnValues(to_sdk_return_value(in.return_values));
    }
    if (in.return_consumed_capacity != ReturnConsumedCapacity::none) {
        request.SetReturnConsumedCapacity(to_sdk_rcc(in.return_consumed_capacity));
    }
    return request;
}

// ---- Transaction request builders (shared by the execute ops + explain) ----
// One Get / Put / Update / Delete / ConditionCheck per inner sub-item; no
// ReturnValues are set on write sub-items (the original never set them in a
// transaction, D-2d-10).

Aws::DynamoDB::Model::TransactGetItemsRequest build_transact_get_items(
    const TransactGetItemsIn& in) {
    Aws::DynamoDB::Model::TransactGetItemsRequest request;
    Aws::Vector<Aws::DynamoDB::Model::TransactGetItem> items;
    items.reserve(in.items.size());
    for (const auto& g : in.items) {
        Aws::DynamoDB::Model::Get get;
        get.SetTableName(g.table_name);
        get.SetKey(to_sdk_item(g.key));
        if (g.projection_expression.has_value()) {
            get.SetProjectionExpression(Aws::String(g.projection_expression->c_str()));
        }
        if (!g.expression_attribute_names.empty()) {
            get.SetExpressionAttributeNames(to_sdk_names(g.expression_attribute_names));
        }
        Aws::DynamoDB::Model::TransactGetItem item;
        item.SetGet(get);
        items.push_back(item);
    }
    request.SetTransactItems(items);
    return request;
}

Aws::DynamoDB::Model::TransactWriteItemsRequest build_transact_write_items(
    const TransactWriteItemsIn& in) {
    Aws::DynamoDB::Model::TransactWriteItemsRequest request;
    Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> items;
    items.reserve(in.items.size());
    for (const auto& sub : in.items) {
        Aws::DynamoDB::Model::TransactWriteItem item;
        std::visit(
            [&](const auto& w) {
                using T = std::decay_t<decltype(w)>;
                if constexpr (std::is_same_v<T, TransactPut>) {
                    Aws::DynamoDB::Model::Put put;
                    put.SetTableName(w.table_name);
                    put.SetItem(to_sdk_item(w.item));
                    if (w.condition_expression.has_value()) {
                        put.SetConditionExpression(Aws::String(w.condition_expression->c_str()));
                    }
                    if (!w.expression_attribute_names.empty()) {
                        put.SetExpressionAttributeNames(to_sdk_names(w.expression_attribute_names));
                    }
                    if (!w.expression_attribute_values.empty()) {
                        put.SetExpressionAttributeValues(
                            to_sdk_item(w.expression_attribute_values));
                    }
                    item.SetPut(put);
                } else if constexpr (std::is_same_v<T, TransactUpdate>) {
                    Aws::DynamoDB::Model::Update update;
                    update.SetTableName(w.table_name);
                    update.SetKey(to_sdk_item(w.key));
                    update.SetUpdateExpression(Aws::String(w.update_expression.c_str()));
                    if (w.condition_expression.has_value()) {
                        update.SetConditionExpression(Aws::String(w.condition_expression->c_str()));
                    }
                    if (!w.expression_attribute_names.empty()) {
                        update.SetExpressionAttributeNames(
                            to_sdk_names(w.expression_attribute_names));
                    }
                    if (!w.expression_attribute_values.empty()) {
                        update.SetExpressionAttributeValues(
                            to_sdk_item(w.expression_attribute_values));
                    }
                    item.SetUpdate(update);
                } else if constexpr (std::is_same_v<T, TransactDelete>) {
                    Aws::DynamoDB::Model::Delete del;
                    del.SetTableName(w.table_name);
                    del.SetKey(to_sdk_item(w.key));
                    if (w.condition_expression.has_value()) {
                        del.SetConditionExpression(Aws::String(w.condition_expression->c_str()));
                    }
                    if (!w.expression_attribute_names.empty()) {
                        del.SetExpressionAttributeNames(to_sdk_names(w.expression_attribute_names));
                    }
                    if (!w.expression_attribute_values.empty()) {
                        del.SetExpressionAttributeValues(
                            to_sdk_item(w.expression_attribute_values));
                    }
                    item.SetDelete(del);
                } else if constexpr (std::is_same_v<T, TransactConditionCheck>) {
                    Aws::DynamoDB::Model::ConditionCheck cc;
                    cc.SetTableName(w.table_name);
                    cc.SetKey(to_sdk_item(w.key));
                    cc.SetConditionExpression(Aws::String(w.condition_expression.c_str()));
                    if (!w.expression_attribute_names.empty()) {
                        cc.SetExpressionAttributeNames(to_sdk_names(w.expression_attribute_names));
                    }
                    if (!w.expression_attribute_values.empty()) {
                        cc.SetExpressionAttributeValues(to_sdk_item(w.expression_attribute_values));
                    }
                    item.SetConditionCheck(cc);
                }
            },
            sub);
        items.push_back(item);
    }
    request.SetTransactItems(items);
    return request;
}

}  // namespace

AwsDDBClient::AwsDDBClient(const std::string& region, const std::string& endpoint) {
    rebuild(region, endpoint);
}

AwsDDBClient::~AwsDDBClient() = default;

void AwsDDBClient::rebuild(const std::string& region, const std::string& endpoint) {
    Aws::Client::ClientConfiguration cfg;
    if (!region.empty()) {
        cfg.region = region;
    }
    if (!endpoint.empty()) {
        cfg.endpointOverride = endpoint;
    }
    // Default-chain credential resolution (honors AWS_PROFILE and a profile's
    // role_arn/source_profile AssumeRole), re-run on every rebuild so a
    // cross-account/region CONNECT re-resolves credentials as the original did.
    client_ = std::make_unique<Aws::DynamoDB::DynamoDBClient>(cfg);
}

Result<void> AwsDDBClient::connect(const std::string& region, const std::string& endpoint) {
    rebuild(region, endpoint);
    return {};
}

Result<GetItemOut> AwsDDBClient::get_item(const GetItemIn& in) {
    auto request = build_get_item(in);

    auto outcome = client_->GetItem(request);
    if (!outcome.IsSuccess()) {
        return fail(from_sdk_error(outcome.GetError()));
    }
    const auto& result = outcome.GetResult();
    GetItemOut out;
    const auto& item = result.GetItem();
    if (!item.empty()) {
        out.item = ItemRow{from_sdk_item(item)};
    }
    out.consumed_capacity = from_sdk_consumed_capacity(result.GetConsumedCapacity());
    return out;
}

Result<QueryOut> AwsDDBClient::query(const QueryIn& in) {
    auto request = build_query(in);

    auto outcome = client_->Query(request);
    if (!outcome.IsSuccess()) {
        return fail(from_sdk_error(outcome.GetError()));
    }
    const auto& result = outcome.GetResult();
    QueryOut out;
    for (const auto& item : result.GetItems()) {
        out.items.push_back(ItemRow{from_sdk_item(item)});
    }
    const auto& lek = result.GetLastEvaluatedKey();
    if (!lek.empty()) {
        out.last_evaluated_key = from_sdk_item(lek);
    }
    out.consumed_capacity = from_sdk_consumed_capacity(result.GetConsumedCapacity());
    return out;
}

Result<ScanOut> AwsDDBClient::scan(const ScanIn& in) {
    auto request = build_scan(in);

    auto outcome = client_->Scan(request);
    if (!outcome.IsSuccess()) {
        return fail(from_sdk_error(outcome.GetError()));
    }
    const auto& result = outcome.GetResult();
    ScanOut out;
    for (const auto& item : result.GetItems()) {
        out.items.push_back(ItemRow{from_sdk_item(item)});
    }
    const auto& lek = result.GetLastEvaluatedKey();
    if (!lek.empty()) {
        out.last_evaluated_key = from_sdk_item(lek);
    }
    out.consumed_capacity = from_sdk_consumed_capacity(result.GetConsumedCapacity());
    return out;
}

Result<PutItemOut> AwsDDBClient::put_item(const PutItemIn& in) {
    auto request = build_put_item(in);

    auto outcome = client_->PutItem(request);
    if (!outcome.IsSuccess()) {
        return fail(from_sdk_error(outcome.GetError()));
    }
    const auto& result = outcome.GetResult();
    PutItemOut out;
    const auto& attrs = result.GetAttributes();
    if (!attrs.empty()) {
        out.attributes = ItemRow{from_sdk_item(attrs)};
    }
    out.consumed_capacity = from_sdk_consumed_capacity(result.GetConsumedCapacity());
    return out;
}

Result<UpdateItemOut> AwsDDBClient::update_item(const UpdateItemIn& in) {
    auto request = build_update_item(in);

    auto outcome = client_->UpdateItem(request);
    if (!outcome.IsSuccess()) {
        return fail(from_sdk_error(outcome.GetError()));
    }
    const auto& result = outcome.GetResult();
    UpdateItemOut out;
    const auto& attrs = result.GetAttributes();
    if (!attrs.empty()) {
        out.attributes = ItemRow{from_sdk_item(attrs)};
    }
    out.consumed_capacity = from_sdk_consumed_capacity(result.GetConsumedCapacity());
    return out;
}

Result<DeleteItemOut> AwsDDBClient::delete_item(const DeleteItemIn& in) {
    auto request = build_delete_item(in);

    auto outcome = client_->DeleteItem(request);
    if (!outcome.IsSuccess()) {
        return fail(from_sdk_error(outcome.GetError()));
    }
    const auto& result = outcome.GetResult();
    DeleteItemOut out;
    const auto& attrs = result.GetAttributes();
    if (!attrs.empty()) {
        out.attributes = ItemRow{from_sdk_item(attrs)};
    }
    out.consumed_capacity = from_sdk_consumed_capacity(result.GetConsumedCapacity());
    return out;
}

namespace {

// Compose the byte-exact transaction-failure message the original logged
// (transact-read.hpp / transact-write.hpp): "Transaction failed. <name>.
// <reqid>. <message>" (D-2d-9). itxt-11 asserts this via a regex. The Error is
// Errc::service_error so the CLI exits nonzero (WILL_FAIL).
Error transact_failed_error(const Aws::Client::AWSError<Aws::DynamoDB::DynamoDBErrors>& err) {
    std::string name(err.GetExceptionName().c_str());
    std::string reqid(err.GetRequestId().c_str());
    std::string message(err.GetMessage().c_str());
    std::string composed = "Transaction failed. " + name + ". " + reqid + ". " + message;
    return Error{Errc::service_error, std::move(composed), std::move(name), std::move(reqid)};
}

}  // namespace

Result<TransactGetItemsOut> AwsDDBClient::transact_get_items(const TransactGetItemsIn& in) {
    auto request = build_transact_get_items(in);
    auto outcome = client_->TransactGetItems(request);
    if (!outcome.IsSuccess()) {
        return fail(transact_failed_error(outcome.GetError()));
    }
    TransactGetItemsOut out;
    for (const auto& response : outcome.GetResult().GetResponses()) {
        const auto& item = response.GetItem();
        if (item.empty()) {
            out.responses.push_back(std::nullopt);
        } else {
            out.responses.push_back(ItemRow{from_sdk_item(item)});
        }
    }
    return out;
}

Result<TransactWriteItemsOut> AwsDDBClient::transact_write_items(const TransactWriteItemsIn& in) {
    auto request = build_transact_write_items(in);
    auto outcome = client_->TransactWriteItems(request);
    if (!outcome.IsSuccess()) {
        return fail(transact_failed_error(outcome.GetError()));
    }
    return TransactWriteItemsOut{};
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

// Build the KeySchemaElement list in canonical [HASH…, RANGE…] order (all
// partition-key elements first in composite order, then all sort-key
// elements). This is the only order that round-trips through DescribeTable and
// the shape DynamoDB expects for composite GSI keys (phase 4a).
Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> to_sdk_key_schema(const KeySchema& ks) {
    Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> out;
    for (const auto& name : ks.hash) {
        Aws::DynamoDB::Model::KeySchemaElement hash;
        hash.SetAttributeName(name);
        hash.SetKeyType(Aws::DynamoDB::Model::KeyType::HASH);
        out.push_back(hash);
    }
    for (const auto& name : ks.range) {
        Aws::DynamoDB::Model::KeySchemaElement range;
        range.SetAttributeName(name);
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

// Build a ProvisionedThroughput from billing rcu/wcu.
Aws::DynamoDB::Model::ProvisionedThroughput to_sdk_provisioned(const BillingModeAndThroughput& b) {
    Aws::DynamoDB::Model::ProvisionedThroughput pt;
    pt.SetReadCapacityUnits(b.rcu);
    pt.SetWriteCapacityUnits(b.wcu);
    return pt;
}

// Build an OnDemandThroughput from the (optional per-dimension) max request
// units. A -1 sentinel (clear-one / UNLIMITED clear-both) is carried verbatim,
// exactly as the original's grammar encoded it (arch §8).
Aws::DynamoDB::Model::OnDemandThroughput to_sdk_ondemand(const OnDemandThroughput& od) {
    Aws::DynamoDB::Model::OnDemandThroughput out;
    if (od.max_read_request_units.has_value()) {
        out.SetMaxReadRequestUnits(*od.max_read_request_units);
    }
    if (od.max_write_request_units.has_value()) {
        out.SetMaxWriteRequestUnits(*od.max_write_request_units);
    }
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
        request.SetProvisionedThroughput(to_sdk_provisioned(*billing));
    } else {
        request.SetBillingMode(Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST);
        request.SetOnDemandThroughput(to_sdk_ondemand(billing->od));
    }
}

// A GSI has no billing MODE of its own (that is a table-level property); it
// carries only the throughput that matches the table's mode. The original's
// gsi production set ProvisionedThroughput / OnDemandThroughput directly.
void apply_gsi_throughput(Aws::DynamoDB::Model::GlobalSecondaryIndex& gsi,
                          const BillingModeAndThroughput& billing) {
    if (billing.mode == BillingMode::provisioned) {
        gsi.SetProvisionedThroughput(to_sdk_provisioned(billing));
    } else {
        gsi.SetOnDemandThroughput(to_sdk_ondemand(billing.od));
    }
}

// Translate one engine::GsiUpdate into a GlobalSecondaryIndexUpdate, mirroring
// the original's alter_table_gsi_create / _update_element / _drop grammar
// actions (parser.y): a Create action carries key schema + projection +
// optional throughput/warm; an Update action carries only throughput (matching
// the table mode) OR warm; a Delete action carries only the index name. The
// create/update throughput setters take the same shape as the table-level ones.
Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate to_sdk_gsi_update(const GsiUpdate& gu) {
    Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate out;
    switch (gu.action) {
        case GsiUpdate::Action::create: {
            Aws::DynamoDB::Model::CreateGlobalSecondaryIndexAction action;
            action.SetIndexName(gu.index_name);
            if (gu.key_schema.has_value()) {
                action.SetKeySchema(to_sdk_key_schema(*gu.key_schema));
            }
            if (gu.projection.has_value()) {
                action.SetProjection(to_sdk_projection(*gu.projection));
            }
            if (gu.billing.has_value()) {
                if (gu.billing->mode == BillingMode::provisioned) {
                    action.SetProvisionedThroughput(to_sdk_provisioned(*gu.billing));
                } else {
                    action.SetOnDemandThroughput(to_sdk_ondemand(gu.billing->od));
                }
            }
            if (gu.warm.has_value()) {
                action.SetWarmThroughput(to_sdk_warm(*gu.warm));
            }
            out.SetCreate(action);
            break;
        }
        case GsiUpdate::Action::update: {
            Aws::DynamoDB::Model::UpdateGlobalSecondaryIndexAction action;
            action.SetIndexName(gu.index_name);
            if (gu.billing.has_value()) {
                if (gu.billing->mode == BillingMode::provisioned) {
                    action.SetProvisionedThroughput(to_sdk_provisioned(*gu.billing));
                } else {
                    action.SetOnDemandThroughput(to_sdk_ondemand(gu.billing->od));
                }
            }
            if (gu.warm.has_value()) {
                action.SetWarmThroughput(to_sdk_warm(*gu.warm));
            }
            out.SetUpdate(action);
            break;
        }
        case GsiUpdate::Action::drop: {
            Aws::DynamoDB::Model::DeleteGlobalSecondaryIndexAction action;
            action.SetIndexName(gu.index_name);
            out.SetDelete(action);
            break;
        }
    }
    return out;
}

// Translate an SSE spec to the SDK SSESpecification (shared by CREATE and
// ALTER). Mirrors the original's sse_specification grammar action.
Aws::DynamoDB::Model::SSESpecification to_sdk_sse(const SseSpec& spec) {
    Aws::DynamoDB::Model::SSESpecification sse;
    sse.SetEnabled(spec.enabled);
    if (spec.enabled) {
        if (spec.sse_type == SseType::aes256) {
            sse.SetSSEType(Aws::DynamoDB::Model::SSEType::AES256);
        } else {
            sse.SetSSEType(Aws::DynamoDB::Model::SSEType::KMS);
        }
        if (spec.kms_master_key_id.has_value()) {
            sse.SetKMSMasterKeyId(*spec.kms_master_key_id);
        }
    }
    return sse;
}

// Translate a stream spec to the SDK StreamSpecification (shared by CREATE and
// ALTER). Mirrors the original's stream_specification grammar action.
Aws::DynamoDB::Model::StreamSpecification to_sdk_stream(const StreamSpec& spec) {
    Aws::DynamoDB::Model::StreamSpecification stream;
    stream.SetStreamEnabled(spec.enabled);
    if (spec.enabled) {
        switch (spec.view_type) {
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
                stream.SetStreamViewType(Aws::DynamoDB::Model::StreamViewType::NEW_AND_OLD_IMAGES);
                break;
        }
    }
    return stream;
}

// Translate a table class to the SDK enum (shared by CREATE and ALTER).
Aws::DynamoDB::Model::TableClass to_sdk_table_class(TableClass tc) {
    return tc == TableClass::standard_infrequent_access
               ? Aws::DynamoDB::Model::TableClass::STANDARD_INFREQUENT_ACCESS
               : Aws::DynamoDB::Model::TableClass::STANDARD;
}

// ---- Vector index / SearchVectors translation (phase 4b) -------------------
// SDK-free VectorIndexDef / SearchVectorsIn -> SDK types. Field names pinned
// against AWS SDK for C++ 1.11.916 (SearchConditionExpression, SearchResults,
// Create/Delete-only VectorIndexUpdate; D-4b-4). The vector attribute is NOT an
// AttributeDefinition (it is an L-of-N, not a scalar key; D-4b-8).
Aws::DynamoDB::Model::VectorDistanceFunction to_sdk_vector_distance(VectorDistance d) {
    switch (d) {
        case VectorDistance::euclidean:
            return Aws::DynamoDB::Model::VectorDistanceFunction::EUCLIDEAN;
        case VectorDistance::dot_product:
            return Aws::DynamoDB::Model::VectorDistanceFunction::DOT_PRODUCT;
        case VectorDistance::cosine:
            break;
    }
    return Aws::DynamoDB::Model::VectorDistanceFunction::COSINE;
}

Aws::Vector<Aws::DynamoDB::Model::SearchSchemaElement> to_sdk_search_schema(
    const std::vector<SearchSchemaElem>& schema) {
    Aws::Vector<Aws::DynamoDB::Model::SearchSchemaElement> out;
    out.reserve(schema.size());
    for (const auto& e : schema) {
        Aws::DynamoDB::Model::SearchSchemaElement se;
        se.SetAttributeName(e.attr);
        se.SetSearchSchemaElementType(
            e.kind == SearchSchemaKind::hash
                ? Aws::DynamoDB::Model::SearchSchemaElementType::HASH
                : Aws::DynamoDB::Model::SearchSchemaElementType::INLINE_FILTER);
        out.push_back(se);
    }
    return out;
}

// Fill the shared vector-index fields on either a VectorIndex (CreateTable) or
// a CreateVectorIndexAction (UpdateTable) — both expose the same setters.
template <class T>
void fill_vector_index(T& obj, const VectorIndexDef& v) {
    obj.SetIndexName(v.index_name);
    Aws::DynamoDB::Model::VectorAttributeDefinition attr;
    attr.SetAttributeName(v.vector_attr);
    obj.SetVectorAttribute(attr);
    obj.SetDimensions(v.dimensions);
    obj.SetDistanceFunction(to_sdk_vector_distance(v.distance));
    if (!v.search_schema.empty()) {
        obj.SetSearchSchema(to_sdk_search_schema(v.search_schema));
    }
    obj.SetProjection(to_sdk_projection(v.projection));
}

Aws::DynamoDB::Model::VectorIndex to_sdk_vector_index(const VectorIndexDef& v) {
    Aws::DynamoDB::Model::VectorIndex out;
    fill_vector_index(out, v);
    return out;
}

Aws::DynamoDB::Model::VectorIndexUpdate to_sdk_vector_index_update(const VectorIndexUpdateDef& u) {
    Aws::DynamoDB::Model::VectorIndexUpdate out;
    if (u.create.has_value()) {
        Aws::DynamoDB::Model::CreateVectorIndexAction action;
        fill_vector_index(action, *u.create);
        out.SetCreate(action);
    } else if (u.drop.has_value()) {
        Aws::DynamoDB::Model::DeleteVectorIndexAction action;
        action.SetIndexName(*u.drop);
        out.SetDelete(action);
    }
    return out;
}

Aws::DynamoDB::Model::SearchVectorsRequest build_search_vectors(const SearchVectorsIn& in) {
    Aws::DynamoDB::Model::SearchVectorsRequest request;
    request.SetTableName(in.table_name);
    request.SetIndexName(in.index_name);
    Aws::Vector<SdkAttributeValue> vec;
    vec.reserve(in.search_vector.size());
    for (const auto& v : in.search_vector) {
        vec.push_back(to_sdk_value(v));
    }
    request.SetSearchVector(std::move(vec));
    request.SetTopK(in.topk);
    if (in.search_condition_expression.has_value()) {
        request.SetSearchConditionExpression(*in.search_condition_expression);
    }
    if (in.projection_expression.has_value()) {
        request.SetProjectionExpression(*in.projection_expression);
    }
    if (!in.expression_attribute_names.empty()) {
        request.SetExpressionAttributeNames(to_sdk_names(in.expression_attribute_names));
    }
    if (!in.expression_attribute_values.empty()) {
        request.SetExpressionAttributeValues(to_sdk_item(in.expression_attribute_values));
    }
    request.SetReturnConsumedCapacity(to_sdk_rcc(in.return_consumed_capacity));
    return request;
}

Aws::DynamoDB::Model::CreateTableRequest build_create_table(const CreateTableIn& in) {
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

    for (const auto& vi : in.vector_indexes) {
        request.AddVectorIndexes(to_sdk_vector_index(vi));
    }

    if (in.stream.has_value()) {
        request.SetStreamSpecification(to_sdk_stream(*in.stream));
    }

    if (in.sse.has_value()) {
        request.SetSSESpecification(to_sdk_sse(*in.sse));
    }

    request.SetTableClass(to_sdk_table_class(in.table_class));

    request.SetDeletionProtectionEnabled(in.deletion_protection);

    for (const auto& tag : in.tags) {
        Aws::DynamoDB::Model::Tag sdk_tag;
        sdk_tag.SetKey(tag.key);
        sdk_tag.SetValue(tag.value);
        request.AddTags(sdk_tag);
    }
    return request;
}

// ---- Replica translation (ddbsh::ReplicaUpdate -> ReplicationGroupUpdate) --
// Port of the original CUpdateTableReplica (arch §7): ADD -> Create, UPDATE ->
// Update, DROP -> Delete, exactly one action per ReplicationGroupUpdate.
// ADD/UPDATE always set the TableClassOverride (the original defaults it to
// STANDARD when the clause is absent — reproduced via value_or) and set the
// read-only ProvisionedThroughputOverride and the per-GSI overrides only when
// the clause provided them.
Aws::DynamoDB::Model::ProvisionedThroughputOverride to_sdk_throughput_override(
    const ProvisionedThroughputOverride& o) {
    Aws::DynamoDB::Model::ProvisionedThroughputOverride out;
    out.SetReadCapacityUnits(o.read_capacity_units);
    return out;
}

Aws::Vector<Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex> to_sdk_replica_gsis(
    const std::vector<ReplicaGsi>& gsis) {
    Aws::Vector<Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex> out;
    out.reserve(gsis.size());
    for (const auto& g : gsis) {
        Aws::DynamoDB::Model::ReplicaGlobalSecondaryIndex rgsi;
        rgsi.SetIndexName(g.index_name);
        rgsi.SetProvisionedThroughputOverride(to_sdk_throughput_override(g.throughput_override));
        out.push_back(rgsi);
    }
    return out;
}

Aws::DynamoDB::Model::ReplicationGroupUpdate to_sdk_replica_update(const ReplicaUpdate& r) {
    Aws::DynamoDB::Model::ReplicationGroupUpdate out;
    switch (r.action) {
        case ReplicaUpdate::Action::drop: {
            Aws::DynamoDB::Model::DeleteReplicationGroupMemberAction del;
            del.SetRegionName(r.region);
            out.SetDelete(del);
            break;
        }
        case ReplicaUpdate::Action::add: {
            Aws::DynamoDB::Model::CreateReplicationGroupMemberAction create;
            create.SetRegionName(r.region);
            create.SetTableClassOverride(
                to_sdk_table_class(r.table_class.value_or(TableClass::standard)));
            if (r.throughput_override.has_value()) {
                create.SetProvisionedThroughputOverride(
                    to_sdk_throughput_override(*r.throughput_override));
            }
            if (!r.replica_gsis.empty()) {
                create.SetGlobalSecondaryIndexes(to_sdk_replica_gsis(r.replica_gsis));
            }
            out.SetCreate(create);
            break;
        }
        case ReplicaUpdate::Action::update: {
            Aws::DynamoDB::Model::UpdateReplicationGroupMemberAction update;
            update.SetRegionName(r.region);
            update.SetTableClassOverride(
                to_sdk_table_class(r.table_class.value_or(TableClass::standard)));
            if (r.throughput_override.has_value()) {
                update.SetProvisionedThroughputOverride(
                    to_sdk_throughput_override(*r.throughput_override));
            }
            if (!r.replica_gsis.empty()) {
                update.SetGlobalSecondaryIndexes(to_sdk_replica_gsis(r.replica_gsis));
            }
            out.SetUpdate(update);
            break;
        }
    }
    return out;
}

// ---- UpdateTable translation (ddbsh::UpdateTableIn -> SDK request) ---------
// Mirrors the original CUpdateTableCommand (arch §7): set ONLY the fields the
// ALTER clause provided, leaving the rest unset so the SDK omits them from the
// payload. Billing always sets BOTH the mode and the matching throughput
// (provisioned => ProvisionedThroughput; on-demand => OnDemandThroughput),
// exactly as the original's billing ctor did.
Aws::DynamoDB::Model::UpdateTableRequest build_update_table(const UpdateTableIn& in) {
    Aws::DynamoDB::Model::UpdateTableRequest request;
    request.SetTableName(in.table_name);

    if (in.billing.has_value()) {
        if (in.billing->mode == BillingMode::provisioned) {
            request.SetBillingMode(Aws::DynamoDB::Model::BillingMode::PROVISIONED);
            request.SetProvisionedThroughput(to_sdk_provisioned(*in.billing));
        } else {
            request.SetBillingMode(Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST);
            request.SetOnDemandThroughput(to_sdk_ondemand(in.billing->od));
        }
    }

    if (in.warm.has_value()) {
        request.SetWarmThroughput(to_sdk_warm(*in.warm));
    }

    if (in.sse.has_value()) {
        request.SetSSESpecification(to_sdk_sse(*in.sse));
    }

    if (in.stream.has_value()) {
        request.SetStreamSpecification(to_sdk_stream(*in.stream));
    }

    if (in.table_class.has_value()) {
        request.SetTableClass(to_sdk_table_class(*in.table_class));
    }

    if (in.deletion_protection.has_value()) {
        request.SetDeletionProtectionEnabled(*in.deletion_protection);
    }

    // CREATE GSI carries the new key attributes in AttributeDefinitions (the
    // SDK requires them on the UpdateTable request); UPDATE/DROP GSI carry none.
    for (const auto& attr : in.attribute_definitions) {
        Aws::DynamoDB::Model::AttributeDefinition def;
        def.SetAttributeName(attr.name);
        def.SetAttributeType(to_sdk(attr.type));
        request.AddAttributeDefinitions(def);
    }

    for (const auto& gu : in.gsi_updates) {
        request.AddGlobalSecondaryIndexUpdates(to_sdk_gsi_update(gu));
    }

    for (const auto& vu : in.vector_index_updates) {
        request.AddVectorIndexUpdates(to_sdk_vector_index_update(vu));
    }

    // A replica ALTER carries exactly one ReplicationGroupUpdate (the original
    // CUpdateTableReplica::run added one). Only a replica ALTER sets this; a
    // plain SET leaves it nullopt (D-2e-12).
    if (in.replica.has_value()) {
        request.AddReplicaUpdates(to_sdk_replica_update(*in.replica));
    }

    return request;
}

Aws::DynamoDB::Model::DeleteTableRequest build_delete_table(const DeleteTableIn& in) {
    Aws::DynamoDB::Model::DeleteTableRequest request;
    request.SetTableName(in.table_name);
    return request;
}

// Mirrors the original CUpdateTableTTLCommand (arch §7): one
// UpdateTimeToLiveRequest carrying a TimeToLiveSpecification{Enabled,
// AttributeName}. The executor supplies the effective attribute name (the
// user's on enable, the existing one on disable), so this is a flat translation.
Aws::DynamoDB::Model::UpdateTimeToLiveRequest build_update_time_to_live(
    const UpdateTimeToLiveIn& in) {
    Aws::DynamoDB::Model::UpdateTimeToLiveRequest request;
    request.SetTableName(in.table_name);
    Aws::DynamoDB::Model::TimeToLiveSpecification spec;
    spec.SetEnabled(in.enabled);
    spec.SetAttributeName(in.attribute_name);
    request.SetTimeToLiveSpecification(spec);
    return request;
}

// Mirrors the original CUpdatePITRCommand (arch §7): one
// UpdateContinuousBackupsRequest carrying a PointInTimeRecoverySpecification
// {PointInTimeRecoveryEnabled}. A flat translation.
Aws::DynamoDB::Model::UpdateContinuousBackupsRequest build_update_continuous_backups(
    const UpdateContinuousBackupsIn& in) {
    Aws::DynamoDB::Model::UpdateContinuousBackupsRequest request;
    request.SetTableName(in.table_name);
    Aws::DynamoDB::Model::PointInTimeRecoverySpecification spec;
    spec.SetPointInTimeRecoveryEnabled(in.enabled);
    request.SetPointInTimeRecoverySpecification(spec);
    return request;
}

// ---- Backup / Restore request builders (2f) --------------------------------

Aws::DynamoDB::Model::CreateBackupRequest build_create_backup(const CreateBackupIn& in) {
    Aws::DynamoDB::Model::CreateBackupRequest request;
    request.SetTableName(in.table_name);
    request.SetBackupName(in.backup_name);
    return request;
}

Aws::DynamoDB::Model::DeleteBackupRequest build_delete_backup(const DeleteBackupIn& in) {
    Aws::DynamoDB::Model::DeleteBackupRequest request;
    request.SetBackupArn(in.backup_arn);
    return request;
}

Aws::DynamoDB::Model::DescribeBackupRequest build_describe_backup(const DescribeBackupIn& in) {
    Aws::DynamoDB::Model::DescribeBackupRequest request;
    request.SetBackupArn(in.backup_arn);
    return request;
}

Aws::DynamoDB::Model::RestoreTableFromBackupRequest build_restore_from_backup(
    const RestoreFromBackupIn& in) {
    Aws::DynamoDB::Model::RestoreTableFromBackupRequest request;
    request.SetTargetTableName(in.target_table_name);
    request.SetBackupArn(in.backup_arn);
    return request;
}

// Build the RestoreTableToPointInTime request. The restore timestamp is parsed
// from ISO-8601 (the original restore_pitr.cpp ctor did the same via
// Aws::Utils::DateTime). `ok` reports whether the parse succeeded so the caller
// can surface a malformed timestamp as an Error (D-2f-4) rather than silently
// sending an invalid DateTime.
Aws::DynamoDB::Model::RestoreTableToPointInTimeRequest build_restore_to_pitr(
    const RestoreToPitrIn& in, bool& ok) {
    Aws::DynamoDB::Model::RestoreTableToPointInTimeRequest request;
    request.SetSourceTableName(in.source_table_name);
    request.SetTargetTableName(in.target_table_name);
    const Aws::Utils::DateTime when(Aws::String(in.restore_datetime.c_str()),
                                    Aws::Utils::DateFormat::ISO_8601);
    ok = when.WasParseSuccessful();
    request.SetRestoreDateTime(when);
    return request;
}

Aws::DynamoDB::Model::DescribeLimitsRequest build_describe_limits(const DescribeLimitsIn&) {
    return Aws::DynamoDB::Model::DescribeLimitsRequest{};
}

Aws::DynamoDB::Model::ListBackupsRequest build_list_backups(const ListBackupsIn& in) {
    Aws::DynamoDB::Model::ListBackupsRequest request;
    if (in.table_name.has_value()) {
        request.SetTableName(*in.table_name);
    }
    if (in.backup_type.has_value()) {
        request.SetBackupType(
            Aws::DynamoDB::Model::BackupTypeFilterMapper::GetBackupTypeFilterForName(
                *in.backup_type));
    }
    request.SetLimit(in.limit);
    if (in.exclusive_start_backup_arn.has_value()) {
        request.SetExclusiveStartBackupArn(*in.exclusive_start_backup_arn);
    }
    return request;
}

}  // namespace

Result<CreateTableOut> AwsDDBClient::create_table(const CreateTableIn& in) {
    auto request = build_create_table(in);

    auto outcome = client_->CreateTable(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return CreateTableOut{};
}

Result<SearchVectorsOut> AwsDDBClient::search_vectors(const SearchVectorsIn& in) {
    auto request = build_search_vectors(in);

    auto outcome = client_->SearchVectors(request);
    if (!outcome.IsSuccess()) {
        return fail(from_sdk_error(outcome.GetError()));
    }
    const auto& result = outcome.GetResult();
    SearchVectorsOut out;
    for (const auto& r : result.GetSearchResults()) {
        SearchVectorsOut::Row row;
        row.item = ItemRow{from_sdk_item(r.GetItem())};
        row.score = r.GetScore();
        out.results.push_back(std::move(row));
    }
    // SearchVectors' ConsumedCapacity is a VectorCapacity (request/write bytes);
    // the Renderer shows it only when the user asked (RETURN CONSUMED CAPACITY).
    const auto& cc = result.GetConsumedCapacity();
    VectorConsumedCapacity vcc;
    vcc.vector_search_request_bytes = cc.GetVectorSearchRequestBytes();
    vcc.vector_write_request_bytes = cc.GetVectorWriteRequestBytes();
    out.vector_capacity = vcc;
    return out;
}

Result<UpdateTableOut> AwsDDBClient::update_table(const UpdateTableIn& in) {
    auto request = build_update_table(in);
    auto outcome = client_->UpdateTable(request);
    if (!outcome.IsSuccess()) {
        // Compose the byte-exact message the original logged on an UpdateTable
        // failure: "Update Table failed. <ExceptionName>. <RequestId>.
        // <Message>" (update_table.cpp logerror, note the space in "Update
        // Table"). Unpinned by golden (icd asserts success paths), D-2e-13.
        const auto& err = outcome.GetError();
        std::string message =
            "Update Table failed. " + std::string(err.GetExceptionName().c_str()) + ". " +
            std::string(err.GetRequestId().c_str()) + ". " + std::string(err.GetMessage().c_str());
        return fail(Error{Errc::service_error, std::move(message),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return UpdateTableOut{};
}

Result<UpdateTimeToLiveOut> AwsDDBClient::update_time_to_live(const UpdateTimeToLiveIn& in) {
    auto request = build_update_time_to_live(in);
    auto outcome = client_->UpdateTimeToLive(request);
    if (!outcome.IsSuccess()) {
        // Compose the byte-exact message the original logged on an
        // UpdateTimeToLive failure: "Error updating TTL. <ExceptionName>.
        // <RequestId>.\n\t<Message>" (update_table_ttl.cpp logerror). Unpinned
        // by golden (ipt/icd assert success paths), D-2e-13.
        const auto& err = outcome.GetError();
        std::string message = "Error updating TTL. " + std::string(err.GetExceptionName().c_str()) +
                              ". " + std::string(err.GetRequestId().c_str()) + ".\n\t" +
                              std::string(err.GetMessage().c_str());
        return fail(Error{Errc::service_error, std::move(message),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return UpdateTimeToLiveOut{};
}

Result<UpdateContinuousBackupsOut> AwsDDBClient::update_continuous_backups(
    const UpdateContinuousBackupsIn& in) {
    auto request = build_update_continuous_backups(in);
    auto outcome = client_->UpdateContinuousBackups(request);
    if (!outcome.IsSuccess()) {
        // Compose the byte-exact message the original logged on an
        // UpdateContinuousBackups failure: "Setting PITR failed. <ExceptionName>.
        // <RequestId>.\n\t<Message>" (pitr.cpp logerror). Unpinned by golden
        // (ipt asserts success paths), D-2e-13.
        const auto& err = outcome.GetError();
        std::string message = "Setting PITR failed. " +
                              std::string(err.GetExceptionName().c_str()) + ". " +
                              std::string(err.GetRequestId().c_str()) + ".\n\t" +
                              std::string(err.GetMessage().c_str());
        return fail(Error{Errc::service_error, std::move(message),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return UpdateContinuousBackupsOut{};
}

// ---- Backup / Restore ops (2f) ---------------------------------------------

Result<CreateBackupOut> AwsDDBClient::create_backup(const CreateBackupIn& in) {
    auto request = build_create_backup(in);
    auto outcome = client_->CreateBackup(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return CreateBackupOut{};
}

Result<DeleteBackupOut> AwsDDBClient::delete_backup(const DeleteBackupIn& in) {
    auto request = build_delete_backup(in);
    auto outcome = client_->DeleteBackup(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return DeleteBackupOut{};
}

Result<DescribeBackupOut> AwsDDBClient::describe_backup(const DescribeBackupIn& in) {
    auto request = build_describe_backup(in);
    auto outcome = client_->DescribeBackup(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    const auto& bd = outcome.GetResult().GetBackupDescription();
    const auto& std_ = bd.GetSourceTableDetails();
    DescribeBackupOut out;
    out.source_table_name = std_.GetTableName().c_str();
    out.source_table_id = std_.GetTableId().c_str();
    out.source_table_arn = std_.GetTableArn().c_str();
    out.table_size_bytes = std_.GetTableSizeBytes();
    out.item_count = std_.GetItemCount();
    for (const auto& kse : std_.GetKeySchema()) {
        out.key_schema.push_back(BackupKeySchemaElement{
            Aws::DynamoDB::Model::KeyTypeMapper::GetNameForKeyType(kse.GetKeyType()).c_str(),
            kse.GetAttributeName().c_str()});
    }
    out.table_creation_datetime =
        std_.GetTableCreationDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str();
    if (std_.BillingModeHasBeenSet()) {
        out.billing_mode =
            Aws::DynamoDB::Model::BillingModeMapper::GetNameForBillingMode(std_.GetBillingMode())
                .c_str();
    }
    if (std_.ProvisionedThroughputHasBeenSet()) {
        out.provisioned = std::make_pair(std_.GetProvisionedThroughput().GetReadCapacityUnits(),
                                         std_.GetProvisionedThroughput().GetWriteCapacityUnits());
    }
    const auto& bdet = bd.GetBackupDetails();
    out.backup_name = bdet.GetBackupName().c_str();
    out.backup_arn = bdet.GetBackupArn().c_str();
    out.backup_size_bytes = bdet.GetBackupSizeBytes();
    out.backup_status =
        Aws::DynamoDB::Model::BackupStatusMapper::GetNameForBackupStatus(bdet.GetBackupStatus())
            .c_str();
    out.backup_type =
        Aws::DynamoDB::Model::BackupTypeMapper::GetNameForBackupType(bdet.GetBackupType()).c_str();
    out.backup_creation_datetime =
        bdet.GetBackupCreationDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str();
    out.backup_expiry_datetime =
        bdet.GetBackupExpiryDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str();
    return out;
}

Result<RestoreFromBackupOut> AwsDDBClient::restore_table_from_backup(
    const RestoreFromBackupIn& in) {
    auto request = build_restore_from_backup(in);
    auto outcome = client_->RestoreTableFromBackup(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return RestoreFromBackupOut{};
}

Result<RestoreToPitrOut> AwsDDBClient::restore_table_to_point_in_time(const RestoreToPitrIn& in) {
    bool parsed = true;
    auto request = build_restore_to_pitr(in, parsed);
    if (!parsed) {
        // A malformed restore timestamp never reaches the service — surface it
        // as an Error (D-2f-4); the library never aborts on user input (D12).
        return fail(Errc::invalid_argument, "Invalid restore timestamp: " + in.restore_datetime);
    }
    auto outcome = client_->RestoreTableToPointInTime(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    return RestoreToPitrOut{};
}

Result<DescribeLimitsOut> AwsDDBClient::describe_limits(const DescribeLimitsIn& in) {
    auto request = build_describe_limits(in);
    auto outcome = client_->DescribeLimits(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    const auto& result = outcome.GetResult();
    DescribeLimitsOut out;
    out.account_max_rcu = result.GetAccountMaxReadCapacityUnits();
    out.account_max_wcu = result.GetAccountMaxWriteCapacityUnits();
    out.table_max_rcu = result.GetTableMaxReadCapacityUnits();
    out.table_max_wcu = result.GetTableMaxWriteCapacityUnits();
    return out;
}

Result<ListBackupsOut> AwsDDBClient::list_backups(const ListBackupsIn& in) {
    auto request = build_list_backups(in);
    auto outcome = client_->ListBackups(request);
    if (!outcome.IsSuccess()) {
        const auto& err = outcome.GetError();
        return fail(Error{Errc::service_error, err.GetMessage().c_str(),
                          std::string(err.GetExceptionName().c_str()),
                          std::string(err.GetRequestId().c_str())});
    }
    const auto& result = outcome.GetResult();
    ListBackupsOut out;
    for (const auto& s : result.GetBackupSummaries()) {
        out.summaries.push_back(BackupSummaryRow{
            s.GetTableName().c_str(), s.GetBackupName().c_str(),
            Aws::DynamoDB::Model::BackupStatusMapper::GetNameForBackupStatus(s.GetBackupStatus())
                .c_str(),
            s.GetBackupArn().c_str(),
            s.GetBackupCreationDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str(),
            s.GetBackupExpiryDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()});
    }
    const std::string cursor = result.GetLastEvaluatedBackupArn().c_str();
    if (!cursor.empty()) {
        out.last_evaluated_backup_arn = cursor;
    }
    return out;
}

Result<DeleteTableOut> AwsDDBClient::delete_table(const DeleteTableIn& in) {
    auto request = build_delete_table(in);
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

// Load a key schema from an SDK KeySchemaElement list into the SDK-free
// TableKeySchema, accumulating every HASH element into hash_keys and every
// RANGE element into range_keys in the SDK's returned (composite) order. A
// composite GSI key (phase 4a) yields multiple HASH and/or RANGE entries.
TableKeySchema load_key_schema(
    const Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement>& key_schema) {
    TableKeySchema out;
    for (const auto& element : key_schema) {
        if (element.GetKeyType() == Aws::DynamoDB::Model::KeyType::HASH) {
            out.hash_keys.emplace_back(element.GetAttributeName().c_str());
        } else if (element.GetKeyType() == Aws::DynamoDB::Model::KeyType::RANGE) {
            out.range_keys.emplace_back(element.GetAttributeName().c_str());
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

// Read a vector index back from DescribeTable (SDK VectorIndexDescription ->
// VectorIndexSchema). The planner classifies a named `.index` as a vector
// index from this list and uses `dimensions` to pre-validate the search-vector
// length; the DESCRIBE / SHOW CREATE TABLE renderers reconstruct the `vsi(…)`
// clause from these fields (D-4b-10).
VectorIndexSchema load_vector_index_detail(
    const Aws::DynamoDB::Model::VectorIndexDescription& vi) {
    VectorIndexSchema out;
    out.index_name = vi.GetIndexName().c_str();
    out.vector_attr = vi.GetVectorAttribute().GetAttributeName().c_str();
    out.dimensions = vi.GetDimensions();
    switch (vi.GetDistanceFunction()) {
        case Aws::DynamoDB::Model::VectorDistanceFunction::EUCLIDEAN:
            out.distance = VectorDistance::euclidean;
            break;
        case Aws::DynamoDB::Model::VectorDistanceFunction::DOT_PRODUCT:
            out.distance = VectorDistance::dot_product;
            break;
        default:
            out.distance = VectorDistance::cosine;
            break;
    }
    for (const auto& se : vi.GetSearchSchema()) {
        SearchSchemaKind kind =
            se.GetSearchSchemaElementType() == Aws::DynamoDB::Model::SearchSchemaElementType::HASH
                ? SearchSchemaKind::hash
                : SearchSchemaKind::inline_filter;
        out.search_schema.push_back(SearchSchemaElem{se.GetAttributeName().c_str(), kind});
    }
    if (vi.ProjectionHasBeenSet()) {
        out.projection = from_sdk_projection(vi.GetProjection());
        out.projection_set = true;
    }
    out.index_status =
        Aws::DynamoDB::Model::IndexStatusMapper::GetNameForIndexStatus(vi.GetIndexStatus()).c_str();
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
    for (const auto& vi : table.GetVectorIndexes()) {
        out.vector_indexes.push_back(load_vector_index_detail(vi));
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
    out.stream_specification_set = table.StreamSpecificationHasBeenSet();
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

Result<std::string> AwsDDBClient::explain_request(const ExplainRequest& request) {
    // Serialize the EXACT SDK request each op would send, without executing it.
    // The body is the SDK's raw SerializePayload() (tab-indented JSON); the
    // Renderer applies the explain_string/strip_newlines transform (P2a-03
    // §4.3). The describe-family requests carry only a table name, so they are
    // built inline here (no shared builder needed).
    auto payload = [](auto&& sdk_request) -> std::string {
        return std::string(sdk_request.SerializePayload().c_str());
    };
    return std::visit(
        [&](const auto& in) -> Result<std::string> {
            using T = std::decay_t<decltype(in)>;
            if constexpr (std::is_same_v<T, GetItemIn>) {
                return payload(build_get_item(in));
            } else if constexpr (std::is_same_v<T, QueryIn>) {
                return payload(build_query(in));
            } else if constexpr (std::is_same_v<T, ScanIn>) {
                return payload(build_scan(in));
            } else if constexpr (std::is_same_v<T, PutItemIn>) {
                return payload(build_put_item(in));
            } else if constexpr (std::is_same_v<T, UpdateItemIn>) {
                return payload(build_update_item(in));
            } else if constexpr (std::is_same_v<T, DeleteItemIn>) {
                return payload(build_delete_item(in));
            } else if constexpr (std::is_same_v<T, CreateTableIn>) {
                return payload(build_create_table(in));
            } else if constexpr (std::is_same_v<T, UpdateTableIn>) {
                return payload(build_update_table(in));
            } else if constexpr (std::is_same_v<T, UpdateTimeToLiveIn>) {
                return payload(build_update_time_to_live(in));
            } else if constexpr (std::is_same_v<T, UpdateContinuousBackupsIn>) {
                return payload(build_update_continuous_backups(in));
            } else if constexpr (std::is_same_v<T, DeleteTableIn>) {
                return payload(build_delete_table(in));
            } else if constexpr (std::is_same_v<T, DescribeTableIn>) {
                Aws::DynamoDB::Model::DescribeTableRequest r;
                r.SetTableName(in.table_name);
                return payload(r);
            } else if constexpr (std::is_same_v<T, DescribeTimeToLiveIn>) {
                Aws::DynamoDB::Model::DescribeTimeToLiveRequest r;
                r.SetTableName(in.table_name);
                return payload(r);
            } else if constexpr (std::is_same_v<T, DescribeContinuousBackupsIn>) {
                Aws::DynamoDB::Model::DescribeContinuousBackupsRequest r;
                r.SetTableName(in.table_name);
                return payload(r);
            } else if constexpr (std::is_same_v<T, DescribeTableReplicaAutoScalingIn>) {
                Aws::DynamoDB::Model::DescribeTableReplicaAutoScalingRequest r;
                r.SetTableName(in.table_name);
                return payload(r);
            } else if constexpr (std::is_same_v<T, TransactGetItemsIn>) {
                return payload(build_transact_get_items(in));
            } else if constexpr (std::is_same_v<T, TransactWriteItemsIn>) {
                return payload(build_transact_write_items(in));
            } else if constexpr (std::is_same_v<T, CreateBackupIn>) {
                return payload(build_create_backup(in));
            } else if constexpr (std::is_same_v<T, DeleteBackupIn>) {
                return payload(build_delete_backup(in));
            } else if constexpr (std::is_same_v<T, DescribeBackupIn>) {
                return payload(build_describe_backup(in));
            } else if constexpr (std::is_same_v<T, RestoreFromBackupIn>) {
                return payload(build_restore_from_backup(in));
            } else if constexpr (std::is_same_v<T, RestoreToPitrIn>) {
                bool parsed = true;
                auto request = build_restore_to_pitr(in, parsed);
                if (!parsed) {
                    return fail(Errc::invalid_argument,
                                "Invalid restore timestamp: " + in.restore_datetime);
                }
                return payload(request);
            } else if constexpr (std::is_same_v<T, SearchVectorsIn>) {
                return payload(build_search_vectors(in));
            } else {
                static_assert(sizeof(T) == 0, "unhandled ExplainRequest alternative");
            }
        },
        request);
}

}  // namespace ddbsh::cli