// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_ddb_client.hpp — the AWS-backed IDDBClient adapter (the ddbsh-aws
// component, alias ddbsh::aws).
//
// SDK BOUNDARY (D5): this header REQUIRES the AWS SDK for C++ — it includes
// <aws/dynamodb/DynamoDBClient.h> and only compiles where the SDK's include
// paths are present. It is deliberately NOT one of the SDK-free engine headers:
// everything under include/ddbsh/*.hpp is SDK-free; everything under
// include/ddbsh/aws/*.hpp pulls the AWS SDK. An embedder who wants only the
// engine links ddbsh::ddbsh and never includes this; an embedder who wants the
// ready-made AWS client links ddbsh::aws and includes this.
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
// (SerializePayload) without issuing it (P2a-03 §4.3). It is the single, shared
// AWS translation used by BOTH the CLI and external embedders.

#ifndef DDBSH_AWS_AWS_DDB_CLIENT_HPP
#define DDBSH_AWS_AWS_DDB_CLIENT_HPP

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <aws/core/client/ClientConfiguration.h>
#include <aws/dynamodb/DynamoDBClient.h>

#include "ddbsh/iddbclient.hpp"

namespace ddbsh::aws {

/// Concrete IDDBClient backed by the AWS SDK for C++.
class AwsDDBClient : public ddbsh::IDDBClient {
public:
    /// @param region    AWS region (empty => SDK default resolution).
    /// @param endpoint  endpoint override (empty => regional endpoint).
    AwsDDBClient(const std::string& region, const std::string& endpoint);

    /// Full-configuration ctor used by ddbsh::aws::open (R-P4d-3). The caller's
    /// `base_config` is the BASE the SDK client builds on (timeouts, retry
    /// strategy, max connections, proxy, …); ddbsh overlays region/endpoint
    /// (non-empty wins) and ALWAYS sets the ddbsh user-agent last (D25), so the
    /// service still sees "ddbsh - version X.Y[.Z]" regardless of the caller's
    /// config. `waiter_timeout` caps how long the CREATE/DROP ACTIVE/gone
    /// waiters poll (a poll-loop budget, SDK-free in nature — D27); nullopt
    /// preserves the original's unbounded backoff loop byte-for-byte. `profile`
    /// (non-empty) resolves credentials from that named profile instead of the
    /// default chain.
    AwsDDBClient(const std::string& region, const std::string& endpoint,
                 const Aws::Client::ClientConfiguration& base_config,
                 std::optional<std::chrono::milliseconds> waiter_timeout = std::nullopt,
                 const std::string& profile = "");

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
    Result<SearchVectorsOut> search_vectors(const SearchVectorsIn&) override;
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
    // same way (D-2j-10). Applies base_config_ (if any) as the base, overlays
    // region/endpoint/user-agent, and honors profile_ for credentials.
    void rebuild(const std::string& region, const std::string& endpoint);

    std::unique_ptr<Aws::DynamoDB::DynamoDBClient> client_;
    std::optional<Aws::Client::ClientConfiguration> base_config_;  // caller's base (R-P4d-3)
    std::optional<std::chrono::milliseconds> waiter_timeout_;      // waiter poll budget (D27)
    std::string profile_;                                          // explicit credential profile
};

}  // namespace ddbsh::aws

#endif  // DDBSH_AWS_AWS_DDB_CLIENT_HPP
