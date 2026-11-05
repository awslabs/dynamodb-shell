// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_api_guard.hpp — RAII wrapper for Aws::InitAPI / Aws::ShutdownAPI (the
// ddbsh-aws component, alias ddbsh::aws).
//
// SDK BOUNDARY (D5): this header REQUIRES the AWS SDK for C++ — it includes
// <aws/core/Aws.h>. Like aws_ddb_client.hpp it lives under include/ddbsh/aws/,
// the SDK-dependent zone, distinct from the SDK-free engine headers under
// include/ddbsh/*.hpp. The AWS SDK lifecycle and all Aws:: types stay out of
// the engine library; an embedder that supplies a non-AWS IDDBClient neither
// links nor initializes the SDK.
//
// Ownership/ordering contract (D4): construct EXACTLY ONE AwsApiGuard — once,
// typically at the top of main() — whose lifetime STRICTLY ENCLOSES every
// AwsDDBClient (and any other AWS SDK use). Shipping it here makes ddbsh::aws a
// complete, self-contained way to stand up the AWS path: an embedder does not
// have to know the Aws::InitAPI / Aws::ShutdownAPI incantation.

#ifndef DDBSH_AWS_AWS_API_GUARD_HPP
#define DDBSH_AWS_AWS_API_GUARD_HPP

#include <aws/core/Aws.h>

namespace ddbsh::aws {

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

}  // namespace ddbsh::aws

#endif  // DDBSH_AWS_AWS_API_GUARD_HPP
