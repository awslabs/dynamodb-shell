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
// ddbsh::Value <-> Aws::AttributeValue translation.

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

    Result<GetItemOut> get_item(const GetItemIn&) override;
    Result<QueryOut> query(const QueryIn&) override;
    Result<ScanOut> scan(const ScanIn&) override;
    Result<PutItemOut> put_item(const PutItemIn&) override;
    Result<CreateTableOut> create_table(const CreateTableIn&) override;
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

private:
    std::unique_ptr<Aws::DynamoDB::DynamoDBClient> client_;
};

}  // namespace ddbsh::cli

#endif  // DDBSH_CLI_AWS_DDB_CLIENT_HPP
