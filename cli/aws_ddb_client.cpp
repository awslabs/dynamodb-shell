// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_ddb_client.cpp — CLI-side IDDBClient adapter (2a subset).
//
// Constructs a real DynamoDBClient with the standard credential resolution and
// optional endpoint override (mirrors the original CDDBShDDBClientConfig, arch
// §3/§10). The four seam ops are stubbed to a clear service_error for 2a: the
// walking-skeleton statements (SHOW VERSION / QUIT) never call them; full
// request/response translation lands in 2d.

#include "aws_ddb_client.hpp"

#include <aws/core/client/ClientConfiguration.h>
#include <aws/dynamodb/model/ListTablesRequest.h>

namespace ddbsh::cli {

namespace {

// The 2a skeleton has no request/response translation yet; the seam ops return
// this clear error. The walking-skeleton statements never reach them.
Error not_yet() {
    return Error{Errc::service_error, "IDDBClient operation not implemented in the 2a skeleton",
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

}  // namespace ddbsh::cli
