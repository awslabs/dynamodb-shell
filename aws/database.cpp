// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// database.cpp — ddbsh::aws::open implementation (P4d, D2/D3/D8).
//
// Builds a Database: acquire the process-wide, reference-counted SDK-init
// token (so the embedder never writes AwsApiGuard and multiple opens compose),
// construct the shared AwsDDBClient from the resolved region/endpoint/config,
// and wrap it in an owning ddbsh::Connection.

#include "ddbsh/aws/database.hpp"

#include <mutex>

#include <aws/core/Aws.h>

#include "ddbsh/aws/aws_ddb_client.hpp"

namespace ddbsh::aws {

namespace {

/// RAII holder for the single process-wide Aws::InitAPI / Aws::ShutdownAPI.
/// Aws::InitAPI/ShutdownAPI are not safely nestable, so open() shares ONE of
/// these via a weak_ptr: the first open creates it (InitAPI), later opens reuse
/// it, and the last Database to drop its shared_ptr runs the dtor (ShutdownAPI).
/// An embedder already holding an AwsApiGuard composes fine (refcounts add).
class SdkInit {
public:
    SdkInit() { Aws::InitAPI(options_); }
    ~SdkInit() { Aws::ShutdownAPI(options_); }
    SdkInit(const SdkInit&) = delete;
    SdkInit& operator=(const SdkInit&) = delete;

private:
    Aws::SDKOptions options_;
};

std::shared_ptr<void> acquire_sdk_token() {
    static std::mutex mu;
    static std::weak_ptr<SdkInit> weak;
    std::lock_guard<std::mutex> lock(mu);
    std::shared_ptr<SdkInit> token = weak.lock();
    if (!token) {
        token = std::make_shared<SdkInit>();
        weak = token;
    }
    return token;
}

}  // namespace

Result<Database> open(const OpenOptions& opts) {
    std::shared_ptr<void> token = acquire_sdk_token();

    std::unique_ptr<IDDBClient> client;
    const std::string profile = opts.profile.value_or("");
    if (opts.client_config.has_value() || opts.waiter_timeout.has_value() || !profile.empty()) {
        client = std::make_unique<AwsDDBClient>(
            opts.region, opts.endpoint,
            opts.client_config.value_or(Aws::Client::ClientConfiguration{}), opts.waiter_timeout,
            profile);
    } else {
        client = std::make_unique<AwsDDBClient>(opts.region, opts.endpoint);
    }

    auto conn =
        std::make_unique<Connection>(SessionConfig{opts.region, opts.endpoint}, std::move(client));
    return Database(std::move(token), std::move(conn));
}

Result<Database> open(const std::string& region, const std::string& endpoint) {
    OpenOptions opts;
    opts.region = region;
    opts.endpoint = endpoint;
    return open(opts);
}

Database::~Database() = default;
Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

}  // namespace ddbsh::aws
