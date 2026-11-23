// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// retry_observer_strategy.cpp — ObservingRetryStrategy and RetryOperationScope
// (P12, D-P12-18; see retry_observer_strategy.hpp for the SDK call order this
// relies on).

#include "retry_observer_strategy.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace ddbsh::aws::detail {
namespace {

// The delay the last CalculateDelayBeforeNextRetry on this thread returned,
// tagged with the strategy and retry count it was computed for. ShouldRetry
// consumes it. The SDK calls the two in the same function on the same thread
// (retry_observer_strategy.hpp), so the tag check is a guard against misuse,
// not a race.
struct PendingDelay {
    const ObservingRetryStrategy* strategy = nullptr;
    long attempted_retries = -1;
    long delay_ms = 0;
};
thread_local PendingDelay t_pending;

thread_local const char* t_operation = "";

// The clock-skew error codes, as the SDK's Aws::Internal::IsClockSkewError
// lists them in 1.11.916 (aws/core/internal/ClockSkew.h). Mirrored here rather
// than included because that header is the SDK's internal surface. For these
// errors the SDK may correct the skew and retry WITHOUT sleeping, so the
// decorator cannot report a delay.
bool is_clock_skew_error(const Aws::Client::AWSError<Aws::Client::CoreErrors>& error) {
    switch (error.GetErrorType()) {
        case Aws::Client::CoreErrors::INVALID_SIGNATURE:
        case Aws::Client::CoreErrors::SIGNATURE_DOES_NOT_MATCH:
        case Aws::Client::CoreErrors::REQUEST_TIME_TOO_SKEWED:
        case Aws::Client::CoreErrors::ACCESS_DENIED:
            return true;
        default:
            return error.GetExceptionName() == "AuthFailure";
    }
}

}  // namespace

// ---- RetryOperationScope ----------------------------------------------------

RetryOperationScope::RetryOperationScope(const char* operation) : previous_(t_operation) {
    t_operation = (operation != nullptr) ? operation : "";
}

RetryOperationScope::~RetryOperationScope() {
    t_operation = previous_;
}

const char* RetryOperationScope::current() {
    return t_operation;
}

// ---- ObservingRetryStrategy -------------------------------------------------

ObservingRetryStrategy::ObservingRetryStrategy(std::shared_ptr<Aws::Client::RetryStrategy> inner,
                                               std::shared_ptr<RetryObserverSlot> slot)
    : inner_(std::move(inner)), slot_(std::move(slot)) {}

long ObservingRetryStrategy::CalculateDelayBeforeNextRetry(
    const Aws::Client::AWSError<Aws::Client::CoreErrors>& error, long attemptedRetries) const {
    const long delay_ms = inner_->CalculateDelayBeforeNextRetry(error, attemptedRetries);
    // Record only. The SDK calls this on every failed attempt, including ones
    // it will not retry, so raising the event here would report retries that
    // never happen (the principal's D-P12-18 amendment).
    t_pending = PendingDelay{this, attemptedRetries, delay_ms};
    return delay_ms;
}

bool ObservingRetryStrategy::ShouldRetry(
    const Aws::Client::AWSError<Aws::Client::CoreErrors>& error, long attemptedRetries) const {
    const bool retry = inner_->ShouldRetry(error, attemptedRetries);
    const PendingDelay pending = t_pending;
    t_pending = PendingDelay{};
    if (!retry) {
        return false;  // a non-retryable error or the last attempt: no event
    }
    ddbsh::RetryObserver* observer = slot_->observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return true;
    }

    ddbsh::RetryEvent event;
    event.operation = RetryOperationScope::current();
    // The SDK passes the number of retries already made; the attempt about to
    // be sent is that plus the first send plus this retry.
    event.attempt = static_cast<int>(attemptedRetries) + 2;
    const long max_attempts = inner_->GetMaxAttempts();
    event.max_attempts = max_attempts > 0 ? static_cast<int>(max_attempts) : 0;
    event.error_code = std::string(error.GetExceptionName().c_str());
    event.throttled = error.ShouldThrottle() || ddbsh::is_throttling_error(event.error_code);
    if (pending.strategy == this && pending.attempted_retries == attemptedRetries &&
        !is_clock_skew_error(error)) {
        event.delay = std::chrono::milliseconds{pending.delay_ms};
    }
    try {
        observer->on_retry(event);
    } catch (...) {
        // The observer is a notification only (retry_event.hpp). An exception
        // must not unwind through the SDK's retry loop, which is not written
        // to survive one; drop it so the retry the SDK decided on still runs.
    }
    return true;
}

long ObservingRetryStrategy::GetMaxAttempts() const {
    return inner_->GetMaxAttempts();
}

void ObservingRetryStrategy::GetSendToken() {
    inner_->GetSendToken();
}

bool ObservingRetryStrategy::HasSendToken() {
    return inner_->HasSendToken();
}

void ObservingRetryStrategy::RequestBookkeeping(
    const Aws::Client::HttpResponseOutcome& httpResponseOutcome) {
    inner_->RequestBookkeeping(httpResponseOutcome);
}

void ObservingRetryStrategy::RequestBookkeeping(
    const Aws::Client::HttpResponseOutcome& httpResponseOutcome,
    const Aws::Client::AWSError<Aws::Client::CoreErrors>& lastError) {
    inner_->RequestBookkeeping(httpResponseOutcome, lastError);
}

const char* ObservingRetryStrategy::GetStrategyName() const {
    // Delegated so the SDK's user-agent retry-mode token is unchanged.
    return inner_->GetStrategyName();
}

}  // namespace ddbsh::aws::detail
