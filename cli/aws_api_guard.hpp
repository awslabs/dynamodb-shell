// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_api_guard.hpp — RAII wrapper for Aws::InitAPI / Aws::ShutdownAPI.
//
// Lives in cli/ ONLY (D11 / human amendment 4): the AWS SDK lifecycle and all
// Aws:: types stay out of the engine library. An embedder that supplies a
// non-AWS IDDBClient neither links nor initializes the SDK.

#ifndef DDBSH_CLI_AWS_API_GUARD_HPP
#define DDBSH_CLI_AWS_API_GUARD_HPP

#include <aws/core/Aws.h>

namespace ddbsh::cli {

/// Initializes the AWS SDK on construction and shuts it down on destruction.
/// Construct exactly one, before any AWS client, at the top of main().
class AwsApiGuard {
public:
    AwsApiGuard() { Aws::InitAPI(options_); }
    ~AwsApiGuard() { Aws::ShutdownAPI(options_); }

    AwsApiGuard(const AwsApiGuard&) = delete;
    AwsApiGuard& operator=(const AwsApiGuard&) = delete;

private:
    Aws::SDKOptions options_;
};

}  // namespace ddbsh::cli

#endif  // DDBSH_CLI_AWS_API_GUARD_HPP
