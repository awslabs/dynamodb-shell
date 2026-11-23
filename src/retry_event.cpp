// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// retry_event.cpp — is_throttling_error (P12, D-P12-18; see retry_event.hpp).

#include "ddbsh/retry_event.hpp"

#include <array>
#include <string_view>

namespace ddbsh {

bool is_throttling_error(std::string_view error_code) {
    // The DynamoDB-reachable subset of the codes the AWS SDK for C++ treats as
    // throttling (aws-cpp-sdk-core CoreErrors.cpp). The browser's app-core.mjs
    // carries the same list, pinned by its own test.
    static constexpr std::array<std::string_view, 6> kThrottlingCodes = {
        "ThrottlingException",    "ProvisionedThroughputExceededException",
        "RequestLimitExceeded",   "TooManyRequestsException",
        "LimitExceededException", "TransactionInProgressException",
    };
    for (const std::string_view code : kThrottlingCodes) {
        if (error_code == code) {
            return true;
        }
    }
    return false;
}

}  // namespace ddbsh
