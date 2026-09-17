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

namespace ddbsh::cli {

namespace {

Result<void> not_yet() {
    return fail(Errc::service_error, "IDDBClient operation not implemented in the 2a skeleton");
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
    return fail(not_yet().error());
}

Result<QueryOut> AwsDDBClient::query(const QueryIn&) {
    return fail(not_yet().error());
}

Result<ScanOut> AwsDDBClient::scan(const ScanIn&) {
    return fail(not_yet().error());
}

Result<PutItemOut> AwsDDBClient::put_item(const PutItemIn&) {
    return fail(not_yet().error());
}

}  // namespace ddbsh::cli
