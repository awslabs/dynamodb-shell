// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_ddb_client.hpp — the CLI-side IDDBClient adapter over the AWS SDK.
//
// This is the SOLE place ddbsh::Value/request structs meet
// Aws::DynamoDB::Model::* (D11). It owns the real DynamoDBClient and translates
// each engine request into an SDK request and each SDK response back into the
// library's SDK-free structs. All in-scope ops are implemented: the
// control-plane ops (create_table / delete_table / describe_table /
// describe_time_to_live / describe_continuous_backups /
// describe_table_replica_auto_scaling / list_tables), the ACTIVE/gone waiters,
// and the data-plane ops (get_item / query / scan / put_item) with full
// ddbsh::Value <-> Aws::AttributeValue translation. It also serves EXPLAIN via
// explain_request(), which serializes the exact SDK request each op would send
// (SerializePayload) without issuing it (P2a-03 §4.3).

#ifndef DDBSH_CLI_AWS_DDB_CLIENT_HPP
#define DDBSH_CLI_AWS_DDB_CLIENT_HPP

#include <memory>
#include <string>

#include <aws/dynamodb/DynamoDBClient.h>

#include "ddbsh/iddbclient.hpp"

namespace ddbsh::cli {

/// Concrete IDDBClient backed by the AWS SDK for C++.
class AwsDDBClient : public ddbsh::IDDBClient {
public:
    /// @param region    AWS region (empty => SDK default resolution).
    /// @param endpoint  endpoint override (empty => regional endpoint).
    AwsDDBClient(const std::string& region, const std::string& endpoint);
    ~AwsDDBClient() override;

    /// Rebuild the underlying SDK client for a new region/endpoint, mirroring
    /// the original reconnect() (arch §3/§7). Credentials re-resolve through the
    /// SDK default chain (which honors AWS_PROFILE + role_arn/source_profile
    /// AssumeRole), so a cross-region or cross-account CONNECT switches both the
    /// endpoint and the credentials, as the original did (B11/B12, D-2j-10).
    /// Returns success; constructing the client performs no network I/O (no
    /// liveness probe — open question #1, default: omit the original's
    /// post-reconnect ListTables, which is not observable in the goldens).
    Result<void> connect(const std::string& region, const std::string& endpoint) override;

    Result<GetItemOut> get_item(const GetItemIn&) override;
    Result<QueryOut> query(const QueryIn&) override;
    Result<ScanOut> scan(const ScanIn&) override;
    Result<PutItemOut> put_item(const PutItemIn&) override;
    Result<UpdateItemOut> update_item(const UpdateItemIn&) override;
    Result<DeleteItemOut> delete_item(const DeleteItemIn&) override;
    Result<TransactGetItemsOut> transact_get_items(const TransactGetItemsIn&) override;
    Result<TransactWriteItemsOut> transact_write_items(const TransactWriteItemsIn&) override;
    Result<CreateTableOut> create_table(const CreateTableIn&) override;
    Result<UpdateTableOut> update_table(const UpdateTableIn&) override;
    Result<UpdateTimeToLiveOut> update_time_to_live(const UpdateTimeToLiveIn&) override;
    Result<UpdateContinuousBackupsOut> update_continuous_backups(
        const UpdateContinuousBackupsIn&) override;
    Result<CreateBackupOut> create_backup(const CreateBackupIn&) override;
    Result<DeleteBackupOut> delete_backup(const DeleteBackupIn&) override;
    Result<DescribeBackupOut> describe_backup(const DescribeBackupIn&) override;
    Result<RestoreFromBackupOut> restore_table_from_backup(const RestoreFromBackupIn&) override;
    Result<RestoreToPitrOut> restore_table_to_point_in_time(const RestoreToPitrIn&) override;
    Result<DescribeLimitsOut> describe_limits(const DescribeLimitsIn&) override;
    Result<ListBackupsOut> list_backups(const ListBackupsIn&) override;
    Result<DeleteTableOut> delete_table(const DeleteTableIn&) override;
    Result<ListTablesOut> list_tables(const ListTablesIn&) override;
    Result<DescribeTableOut> describe_table(const DescribeTableIn&) override;
    Result<TimeToLiveOut> describe_time_to_live(const DescribeTimeToLiveIn&) override;
    Result<ContinuousBackupsOut> describe_continuous_backups(
        const DescribeContinuousBackupsIn&) override;
    Result<TableReplicaAutoScalingOut> describe_table_replica_auto_scaling(
        const DescribeTableReplicaAutoScalingIn&) override;
    Result<void> wait_for_table_gone(const std::string& table_name) override;
    Result<void> wait_for_table_active(const std::string& table_name) override;
    Result<std::string> explain_request(const ExplainRequest&) override;

private:
    // Construct (or replace) the underlying SDK client for a region/endpoint.
    // Shared by the ctor and connect() so both resolve credentials/endpoint the
    // same way (D-2j-10).
    void rebuild(const std::string& region, const std::string& endpoint);

    std::unique_ptr<Aws::DynamoDB::DynamoDBClient> client_;
};

}  // namespace ddbsh::cli

#endif  // DDBSH_CLI_AWS_DDB_CLIENT_HPP
