// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// retry_observer_strategy.hpp — the AWS SDK RetryStrategy decorator that turns
// the SDK's retries into ddbsh::RetryEvents (P12, D-P12-18 as amended by the
// principal, "Principal Review — c940d4c").
//
// INTERNAL to the ddbsh-aws component: not installed, not part of the public
// surface. AwsDDBClient::rebuild wraps the configured RetryStrategy in an
// ObservingRetryStrategy; the unit test drives it directly against a fake
// inner strategy.
//
// How the AWS SDK for C++ 1.11.916 calls a RetryStrategy after a failed
// attempt (smithy AwsSmithyClientBase::HandleAsyncReply, the path the
// DynamoDBClient uses; AWSClient::AttemptExhaustively has the same order):
//
//   1. CalculateDelayBeforeNextRetry(error, retries) — on EVERY failed
//      attempt, retryable or not, including the last one;
//   2. AdjustClockSkew — when the error is a clock-skew error and the skew
//      exceeds the threshold, the SDK rewrites the error as retryable and
//      will NOT sleep before the retry;
//   3. ShouldRetry(error, retries) — skipped only on the region-correction
//      path (an AWS_GLOBAL client redirected to another region), which
//      retries without sleeping;
//   4. if ShouldRetry returned true: sleep the step-1 delay (unless step 2 or
//      the region path suppressed it), then send again.
//
// So the decorator records the step-1 delay in a per-thread slot and raises
// the event from ShouldRetry, only when the inner strategy said yes. Steps 1
// and 3 run in one function on one thread, so the slot always holds the delay
// computed for the same failure. The cases this leaves out:
//   - a clock-skew error: the event is raised, but `delay` is nullopt, since
//     the decorator cannot see whether step 2 suppressed the sleep;
//   - the region-correction path: no event (ShouldRetry is not called). It
//     applies only to an AWS_GLOBAL client, which ddbsh never builds;
//   - an event-stream request that the legacy AWSClient abandons after
//     ShouldRetry: DynamoDB has no event-stream operations.

#ifndef DDBSH_AWS_RETRY_OBSERVER_STRATEGY_HPP
#define DDBSH_AWS_RETRY_OBSERVER_STRATEGY_HPP

#include <atomic>
#include <memory>
#include <string>

#include <aws/core/client/AWSError.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/client/RetryStrategy.h>

#include "ddbsh/retry_event.hpp"

namespace ddbsh::aws::detail {

/// The observer registration shared by every strategy an AwsDDBClient builds.
/// AwsDDBClient owns one for its lifetime and hands it to each rebuilt
/// strategy, so a registration survives connect() and the SDK never holds a
/// pointer into a destroyed client.
struct RetryObserverSlot {
    std::atomic<ddbsh::RetryObserver*> observer{nullptr};
};

/// Names the operation the current thread is sending, for RetryEvent::operation.
/// AwsDDBClient opens one around each SDK call. Scopes nest; the innermost
/// name wins, and the previous name is restored on exit.
class RetryOperationScope {
public:
    explicit RetryOperationScope(const char* operation);
    ~RetryOperationScope();
    RetryOperationScope(const RetryOperationScope&) = delete;
    RetryOperationScope& operator=(const RetryOperationScope&) = delete;

    /// The current thread's operation name, or "" outside every scope.
    static const char* current();

private:
    const char* previous_;
};

/// A RetryStrategy that delegates every decision to `inner` and reports each
/// retry the inner strategy approves to the slot's observer.
class ObservingRetryStrategy : public Aws::Client::RetryStrategy {
public:
    ObservingRetryStrategy(std::shared_ptr<Aws::Client::RetryStrategy> inner,
                           std::shared_ptr<RetryObserverSlot> slot);

    bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors>& error,
                     long attemptedRetries) const override;
    long CalculateDelayBeforeNextRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors>& error,
                                       long attemptedRetries) const override;
    long GetMaxAttempts() const override;
    void GetSendToken() override;
    bool HasSendToken() override;
    void RequestBookkeeping(const Aws::Client::HttpResponseOutcome& httpResponseOutcome) override;
    void RequestBookkeeping(
        const Aws::Client::HttpResponseOutcome& httpResponseOutcome,
        const Aws::Client::AWSError<Aws::Client::CoreErrors>& lastError) override;
    const char* GetStrategyName() const override;

    /// The wrapped strategy (for tests and for AwsDDBClient's own checks).
    const std::shared_ptr<Aws::Client::RetryStrategy>& inner() const { return inner_; }

private:
    std::shared_ptr<Aws::Client::RetryStrategy> inner_;
    std::shared_ptr<RetryObserverSlot> slot_;
};

}  // namespace ddbsh::aws::detail

#endif  // DDBSH_AWS_RETRY_OBSERVER_STRATEGY_HPP
