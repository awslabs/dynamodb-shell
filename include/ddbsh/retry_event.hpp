// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// retry_event.hpp — the transport retry/throttle notification (P12, D-P12-18).
//
// DynamoDB throttles and occasionally fails a request with a retryable error;
// the transport under the IDDBClient seam (the AWS SDK for C++ retry strategy
// in ddbsh::aws, the WireClient retry loop in ddbsh::wire) then backs off and
// sends the request again. The engine never sees those retries. This header
// lets an embedder watch them: it registers a RetryObserver on the client
// (IDDBClient::set_retry_observer, or Connection::set_retry_observer), and the
// transport calls on_retry once for every retry it has decided to make, before
// it sleeps the backoff.
//
// An event means a retry WILL be attempted. A transport never raises one for a
// non-retryable error or for the last attempt, after which it gives up.
//
// SDK-free and terminal-free: this is part of the engine's public surface
// (include/ddbsh/*.hpp). What an embedder does with an event (a stderr line in
// the CLI's --show-retries, a busy-indicator state in the web UI) is the
// embedder's policy.

#ifndef DDBSH_RETRY_EVENT_HPP
#define DDBSH_RETRY_EVENT_HPP

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace ddbsh {

/// One retry a transport has decided to make, raised before its backoff.
struct RetryEvent {
    /// The DynamoDB operation being retried ("Scan", "DescribeTable", ...).
    /// Empty when the transport does not know it.
    std::string operation;
    /// The attempt about to be made, counting the first send as 1: the first
    /// retry is attempt 2.
    int attempt = 0;
    /// The transport's attempt budget (first send included); 0 when the
    /// transport does not know it.
    int max_attempts = 0;
    /// The error that caused the retry, e.g.
    /// "ProvisionedThroughputExceededException". Empty when the failure carried
    /// no exception name (for example a network failure).
    std::string error_code;
    /// True when the error is a throttling error: `error_code` satisfies
    /// is_throttling_error, or the transport itself classified the error as
    /// throttling.
    bool throttled = false;
    /// The backoff the transport is about to sleep before the retry. nullopt
    /// when the transport cannot say for certain how long it will wait (the AWS
    /// SDK skips the sleep after correcting a clock skew).
    std::optional<std::chrono::milliseconds> delay;
};

/// An embedder's receiver for RetryEvents. The transport calls on_retry on
/// the thread that issued the request, synchronously, before it sleeps; a slow
/// observer delays the retry. on_retry must not call back into the client and
/// should not throw: the transport catches and discards anything it throws, so
/// the retry it has decided on still happens. Keep the observer alive until it
/// is cleared (set_retry_observer(nullptr)) and no call to on_retry is still
/// running. AwsDDBClient and WireClient hold the registration atomically, so
/// either may be set or cleared from another thread while a request is
/// retrying; clearing does not wait for an on_retry call already running on
/// the requesting thread. A custom IDDBClient states its own rule.
class RetryObserver {
public:
    virtual ~RetryObserver() = default;
    /// Called once per retry, before the backoff.
    virtual void on_retry(const RetryEvent& event) = 0;
};

/// True when `error_code` names a DynamoDB throttling error. The list holds the
/// DynamoDB-reachable codes the AWS SDK for C++ classifies as throttling:
/// ThrottlingException, ProvisionedThroughputExceededException,
/// RequestLimitExceeded, TooManyRequestsException, LimitExceededException and
/// TransactionInProgressException. Matching is exact and case-sensitive; any
/// prefix such as "com.amazonaws.dynamodb.v20120810#" must already be removed.
bool is_throttling_error(std::string_view error_code);

}  // namespace ddbsh

#endif  // DDBSH_RETRY_EVENT_HPP
