// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_ddb_client.hpp — the CLI-side IDDBClient adapter over the AWS SDK.
//
// This is the SOLE place ddbsh::Value/request structs meet
// Aws::DynamoDB::Model::* (D11). It owns the real DynamoDBClient and translates
// each engine request into an SDK request and each SDK response back into the
// library's SDK-free structs.
//
// 2a scope: constructs the client and implements the four seam ops the walking
// skeleton / near-term ports declare. Full request shaping and Value
// translation land per sub-phase (2d); those 2a implementations return a
// service_error placeholder until then (the walking statements do not call
// them). list_tables() is the exception: it is fully implemented here so the
// 2a readiness-gate smoke program can prove real backend reachability.

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
    Result<ListTablesOut> list_tables(const ListTablesIn&) override;

private:
    std::unique_ptr<Aws::DynamoDB::DynamoDBClient> client_;
};

}  // namespace ddbsh::cli

#endif  // DDBSH_CLI_AWS_DDB_CLIENT_HPP
