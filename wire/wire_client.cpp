// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_client.cpp — WireClient implementation (P6b; see wire_client.hpp).
//
// Each data-plane op is the same three-step pipeline: serialize the request
// (P6a), send it through the Transport, parse the response (P6b, maps a
// `__type` document to an Error). explain_request std::visits the ExplainRequest
// variant and returns the serialized body for a data-plane alternative,
// not_implemented otherwise. The remaining IDDBClient virtuals are labelled P6c
// stubs. SDK-free + terminal-free (the wire boundary rule).

#include "ddbsh/wire/wire_client.hpp"

#include <string>
#include <type_traits>
#include <variant>

#include "ddbsh/wire/codec.hpp"

namespace ddbsh::wire {
namespace {

// A labelled P6c stub: an op the wire client does not yet support.
std::unexpected<Error> not_yet(const char* op) {
    return fail(Errc::not_implemented,
                std::string(op) + " is not yet supported by the wire client (P6c)");
}

}  // namespace

Result<void> WireClient::connect(const std::string& region, const std::string& endpoint) {
    region_ = region;
    endpoint_ = endpoint;
    return {};
}

Result<GetItemOut> WireClient::get_item(const GetItemIn& in) {
    const WireRequest req = serialize(in);
    auto body = transport_.send(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    return parse_get_item(*body);
}

Result<QueryOut> WireClient::query(const QueryIn& in) {
    const WireRequest req = serialize(in);
    auto body = transport_.send(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    return parse_query(*body);
}

Result<ScanOut> WireClient::scan(const ScanIn& in) {
    const WireRequest req = serialize(in);
    auto body = transport_.send(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    return parse_scan(*body);
}

Result<PutItemOut> WireClient::put_item(const PutItemIn& in) {
    const WireRequest req = serialize(in);
    auto body = transport_.send(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    return parse_put_item(*body);
}

Result<UpdateItemOut> WireClient::update_item(const UpdateItemIn& in) {
    const WireRequest req = serialize(in);
    auto body = transport_.send(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    return parse_update_item(*body);
}

Result<DeleteItemOut> WireClient::delete_item(const DeleteItemIn& in) {
    const WireRequest req = serialize(in);
    auto body = transport_.send(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    return parse_delete_item(*body);
}

Result<std::string> WireClient::explain_request(const ExplainRequest& req) {
    // The six data-plane alternatives serialize SDK-free (P6a); the EXPLAIN body
    // is exactly the serialized request body (P6-02 §3.2), byte-proven == the
    // SDK's SerializePayload() by the P6a differential gate. Every other
    // alternative awaits the P6c serializers.
    return std::visit(
        [](const auto& in) -> Result<std::string> {
            using T = std::decay_t<decltype(in)>;
            if constexpr (std::is_same_v<T, GetItemIn> || std::is_same_v<T, QueryIn> ||
                          std::is_same_v<T, ScanIn> || std::is_same_v<T, PutItemIn> ||
                          std::is_same_v<T, UpdateItemIn> || std::is_same_v<T, DeleteItemIn>) {
                return serialize(in).body;
            } else {
                return not_yet("EXPLAIN for this request");
            }
        },
        req);
}

// ---- P6c stubs -------------------------------------------------------------

Result<CreateTableOut> WireClient::create_table(const CreateTableIn&) {
    return not_yet("CREATE TABLE");
}
Result<UpdateTableOut> WireClient::update_table(const UpdateTableIn&) {
    return not_yet("ALTER TABLE");
}
Result<UpdateTimeToLiveOut> WireClient::update_time_to_live(const UpdateTimeToLiveIn&) {
    return not_yet("ALTER TABLE SET TTL");
}
Result<UpdateContinuousBackupsOut> WireClient::update_continuous_backups(
    const UpdateContinuousBackupsIn&) {
    return not_yet("ALTER TABLE SET PITR");
}
Result<CreateBackupOut> WireClient::create_backup(const CreateBackupIn&) {
    return not_yet("BACKUP TABLE");
}
Result<DeleteBackupOut> WireClient::delete_backup(const DeleteBackupIn&) {
    return not_yet("DROP BACKUP");
}
Result<DescribeBackupOut> WireClient::describe_backup(const DescribeBackupIn&) {
    return not_yet("DESCRIBE BACKUP");
}
Result<RestoreFromBackupOut> WireClient::restore_table_from_backup(const RestoreFromBackupIn&) {
    return not_yet("RESTORE TABLE FROM BACKUP");
}
Result<RestoreToPitrOut> WireClient::restore_table_to_point_in_time(const RestoreToPitrIn&) {
    return not_yet("RESTORE TABLE TO PITR");
}
Result<DescribeLimitsOut> WireClient::describe_limits(const DescribeLimitsIn&) {
    return not_yet("SHOW LIMITS");
}
Result<ListBackupsOut> WireClient::list_backups(const ListBackupsIn&) {
    return not_yet("SHOW BACKUPS");
}
Result<DeleteTableOut> WireClient::delete_table(const DeleteTableIn&) {
    return not_yet("DROP TABLE");
}
Result<ListTablesOut> WireClient::list_tables(const ListTablesIn&) {
    return not_yet("SHOW TABLES");
}
Result<DescribeTableOut> WireClient::describe_table(const DescribeTableIn&) {
    return not_yet("DESCRIBE");
}
Result<TransactGetItemsOut> WireClient::transact_get_items(const TransactGetItemsIn&) {
    return not_yet("read transaction");
}
Result<TransactWriteItemsOut> WireClient::transact_write_items(const TransactWriteItemsIn&) {
    return not_yet("write transaction");
}
Result<TimeToLiveOut> WireClient::describe_time_to_live(const DescribeTimeToLiveIn&) {
    return not_yet("DescribeTimeToLive");
}
Result<ContinuousBackupsOut> WireClient::describe_continuous_backups(
    const DescribeContinuousBackupsIn&) {
    return not_yet("DescribeContinuousBackups");
}
Result<TableReplicaAutoScalingOut> WireClient::describe_table_replica_auto_scaling(
    const DescribeTableReplicaAutoScalingIn&) {
    return not_yet("DescribeTableReplicaAutoScaling");
}
Result<void> WireClient::wait_for_table_active(const std::string&) {
    return not_yet("table-active waiter");
}
Result<void> WireClient::wait_for_table_gone(const std::string&) {
    return not_yet("table-gone waiter");
}

}  // namespace ddbsh::wire
