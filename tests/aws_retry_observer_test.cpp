// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_retry_observer_test.cpp — the AWS SDK retry decorator (P12 test 6, the
// C++ SDK part; D-P12-18 as amended in "Principal Review — c940d4c").
//
// Two layers:
//   1. ObservingRetryStrategy against a fake inner strategy, offline: every
//      virtual delegates; an event is raised only when the inner ShouldRetry
//      says yes (no event for a non-retryable error or for the exhausted final
//      attempt); the delay is the one CalculateDelayBeforeNextRetry recorded;
//      attempt = retries + 2; the operation comes from RetryOperationScope;
//      throttled follows the SDK's own classification and is_throttling_error;
//      a clock-skew error reports no delay; nullptr and swapped observers.
//   2. AwsDDBClient end to end against a loopback port nothing listens on
//      (127.0.0.1:1, connection refused, a retryable SDK network error), with
//      the fake inner strategy supplied through base_config: rebuild() wraps
//      the caller's strategy, the event names the operation, and the
//      registration survives connect(). The same end-to-end path then runs
//      with no retryStrategy in the base configuration and with the
//      two-argument constructor (the two ways the CLI builds its client), so
//      rebuild() wraps the DynamoDB client's own default strategy and the
//      attempt budget equals that default's. No AWS service is contacted;
//      fixed dummy credentials keep the SDK off the credential chain's network
//      providers.
//
// The binary has its own main() (one AwsApiGuard for the process; dummy
// credentials set before InitAPI), so it links GTest::gtest.

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <aws/core/client/AWSError.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/client/RetryStrategy.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/Outcome.h>
#include <aws/dynamodb/DynamoDBClientConfiguration.h>

#include "ddbsh/aws/aws_api_guard.hpp"
#include "ddbsh/aws/aws_ddb_client.hpp"
#include "ddbsh/retry_event.hpp"

#include "retry_observer_strategy.hpp"  // aws/ (the component's internal header)

namespace {

using Aws::Client::AWSError;
using Aws::Client::CoreErrors;
using Aws::Client::RetryableType;
using ddbsh::aws::detail::ObservingRetryStrategy;
using ddbsh::aws::detail::RetryObserverSlot;
using ddbsh::aws::detail::RetryOperationScope;

// A scripted inner strategy that counts every call. It retries while
// attemptedRetries < retries_allowed and the error is retryable, and returns
// `delay_ms` (plus the retry count, so each delay is distinct).
class FakeStrategy : public Aws::Client::RetryStrategy {
public:
    explicit FakeStrategy(long retries_allowed = 2, long delay_ms = 100)
        : retries_allowed_(retries_allowed), delay_ms_(delay_ms) {}

    bool ShouldRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const override {
        ++should_retry_calls;
        return error.ShouldRetry() && attemptedRetries < retries_allowed_;
    }
    long CalculateDelayBeforeNextRetry(const AWSError<CoreErrors>& /*error*/,
                                       long attemptedRetries) const override {
        ++delay_calls;
        return delay_ms_ + attemptedRetries;
    }
    long GetMaxAttempts() const override {
        ++max_attempts_calls;
        return retries_allowed_ + 1;
    }
    void GetSendToken() override { ++get_send_token_calls; }
    bool HasSendToken() override {
        ++has_send_token_calls;
        return has_send_token_result;
    }
    void RequestBookkeeping(const Aws::Client::HttpResponseOutcome& /*outcome*/) override {
        ++bookkeeping1_calls;
    }
    void RequestBookkeeping(const Aws::Client::HttpResponseOutcome& /*outcome*/,
                            const AWSError<CoreErrors>& /*lastError*/) override {
        ++bookkeeping2_calls;
    }
    const char* GetStrategyName() const override { return "fake-strategy"; }

    mutable int should_retry_calls = 0;
    mutable int delay_calls = 0;
    mutable int max_attempts_calls = 0;
    int get_send_token_calls = 0;
    int has_send_token_calls = 0;
    bool has_send_token_result = true;
    int bookkeeping1_calls = 0;
    int bookkeeping2_calls = 0;

private:
    long retries_allowed_;
    long delay_ms_;
};

class RecordingObserver : public ddbsh::RetryObserver {
public:
    void on_retry(const ddbsh::RetryEvent& event) override { events.push_back(event); }
    std::vector<ddbsh::RetryEvent> events;
};

class ThrowingObserver : public ddbsh::RetryObserver {
public:
    void on_retry(const ddbsh::RetryEvent& /*event*/) override {
        ++calls;
        throw std::runtime_error("observer failure");
    }
    int calls = 0;
};

AWSError<CoreErrors> throttle_error() {
    return AWSError<CoreErrors>(CoreErrors::THROTTLING, RetryableType::RETRYABLE_THROTTLING,
                                "ThrottlingException", "Rate exceeded");
}

// Drive one failed attempt the way the SDK does: CalculateDelay, then
// ShouldRetry, for the same error and retry count.
bool sdk_attempt(ObservingRetryStrategy& s, const AWSError<CoreErrors>& error, long retries) {
    (void)s.CalculateDelayBeforeNextRetry(error, retries);
    return s.ShouldRetry(error, retries);
}

struct Fixture {
    std::shared_ptr<FakeStrategy> inner = std::make_shared<FakeStrategy>();
    std::shared_ptr<RetryObserverSlot> slot = std::make_shared<RetryObserverSlot>();
    ObservingRetryStrategy strategy{inner, slot};
    RecordingObserver observer;
    Fixture() { slot->observer.store(&observer); }
};

// ---- layer 1: the decorator against a fake inner strategy --------------------

TEST(ObservingRetryStrategy, EveryVirtualDelegates) {
    Fixture f;
    const AWSError<CoreErrors> err = throttle_error();
    const Aws::Client::HttpResponseOutcome outcome(err);

    EXPECT_EQ(f.strategy.CalculateDelayBeforeNextRetry(err, 1), 101);
    EXPECT_TRUE(f.strategy.ShouldRetry(err, 1));
    EXPECT_EQ(f.strategy.GetMaxAttempts(), 3);
    f.strategy.GetSendToken();
    f.inner->has_send_token_result = false;
    EXPECT_FALSE(f.strategy.HasSendToken());  // the inner's answer, not the base default
    f.inner->has_send_token_result = true;
    EXPECT_TRUE(f.strategy.HasSendToken());
    f.strategy.RequestBookkeeping(outcome);
    f.strategy.RequestBookkeeping(outcome, err);
    EXPECT_STREQ(f.strategy.GetStrategyName(), "fake-strategy");

    EXPECT_EQ(f.inner->delay_calls, 1);
    EXPECT_EQ(f.inner->should_retry_calls, 1);
    EXPECT_GE(f.inner->max_attempts_calls, 1);
    EXPECT_EQ(f.inner->get_send_token_calls, 1);
    EXPECT_EQ(f.inner->has_send_token_calls, 2);
    EXPECT_EQ(f.inner->bookkeeping1_calls, 1);
    EXPECT_EQ(f.inner->bookkeeping2_calls, 1);
}

TEST(ObservingRetryStrategy, ApprovedRetryRaisesOneEventWithTheRecordedDelay) {
    Fixture f;
    const RetryOperationScope scope("Scan");

    ASSERT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));
    ASSERT_TRUE(sdk_attempt(f.strategy, throttle_error(), 1));

    ASSERT_EQ(f.observer.events.size(), 2u);
    const ddbsh::RetryEvent& first = f.observer.events[0];
    EXPECT_EQ(first.operation, "Scan");
    EXPECT_EQ(first.attempt, 2);  // retries already made (0) + 2
    EXPECT_EQ(first.max_attempts, 3);
    EXPECT_EQ(first.error_code, "ThrottlingException");
    EXPECT_TRUE(first.throttled);
    ASSERT_TRUE(first.delay.has_value());
    EXPECT_EQ(first.delay->count(), 100);
    EXPECT_EQ(f.observer.events[1].attempt, 3);
    ASSERT_TRUE(f.observer.events[1].delay.has_value());
    EXPECT_EQ(f.observer.events[1].delay->count(), 101);
}

TEST(ObservingRetryStrategy, NonRetryableErrorRaisesNoEvent) {
    // The SDK calls CalculateDelayBeforeNextRetry for this failure too; the
    // decorator must not report it (the A3 error the principal caught).
    Fixture f;
    const AWSError<CoreErrors> ccf(CoreErrors::UNKNOWN, RetryableType::NOT_RETRYABLE,
                                   "ConditionalCheckFailedException",
                                   "The conditional request failed");
    EXPECT_FALSE(sdk_attempt(f.strategy, ccf, 0));
    EXPECT_EQ(f.inner->delay_calls, 1);
    EXPECT_TRUE(f.observer.events.empty());
}

TEST(ObservingRetryStrategy, ExhaustedFinalAttemptRaisesNoEvent) {
    Fixture f;  // two retries allowed: attempts 1..3
    EXPECT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));
    EXPECT_TRUE(sdk_attempt(f.strategy, throttle_error(), 1));
    EXPECT_FALSE(sdk_attempt(f.strategy, throttle_error(), 2));  // the SDK gives up
    ASSERT_EQ(f.observer.events.size(), 2u);
    EXPECT_EQ(f.observer.events.back().attempt, 3);
}

TEST(ObservingRetryStrategy, OperationComesFromTheInnermostScope) {
    Fixture f;
    EXPECT_STREQ(RetryOperationScope::current(), "");
    {
        const RetryOperationScope outer("DescribeTable");
        {
            const RetryOperationScope inner("Query");
            ASSERT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));
        }
        EXPECT_STREQ(RetryOperationScope::current(), "DescribeTable");
        ASSERT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));
    }
    EXPECT_STREQ(RetryOperationScope::current(), "");
    ASSERT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));  // outside every scope

    ASSERT_EQ(f.observer.events.size(), 3u);
    EXPECT_EQ(f.observer.events[0].operation, "Query");
    EXPECT_EQ(f.observer.events[1].operation, "DescribeTable");
    EXPECT_EQ(f.observer.events[2].operation, "");
}

TEST(ObservingRetryStrategy, ThrottledFollowsTheSdkAndTheCodeList) {
    Fixture f;
    // The SDK's classification alone (an exception name not on the list).
    const AWSError<CoreErrors> sdk_throttle(CoreErrors::SLOW_DOWN,
                                            RetryableType::RETRYABLE_THROTTLING, "SlowDown", "");
    // The code list alone (a plain retryable error with a throttling name).
    const AWSError<CoreErrors> named(CoreErrors::UNKNOWN, RetryableType::RETRYABLE,
                                     "ProvisionedThroughputExceededException", "");
    // Neither.
    const AWSError<CoreErrors> transient(CoreErrors::INTERNAL_FAILURE, RetryableType::RETRYABLE,
                                         "InternalServerError", "");
    ASSERT_TRUE(sdk_attempt(f.strategy, sdk_throttle, 0));
    ASSERT_TRUE(sdk_attempt(f.strategy, named, 0));
    ASSERT_TRUE(sdk_attempt(f.strategy, transient, 0));
    ASSERT_EQ(f.observer.events.size(), 3u);
    EXPECT_TRUE(f.observer.events[0].throttled);
    EXPECT_TRUE(f.observer.events[1].throttled);
    EXPECT_FALSE(f.observer.events[2].throttled);
    EXPECT_EQ(f.observer.events[2].error_code, "InternalServerError");
}

TEST(ObservingRetryStrategy, ClockSkewRetryReportsNoDelay) {
    // After correcting a clock skew the SDK retries without sleeping, and the
    // decorator cannot tell whether it did, so it reports no delay.
    Fixture f;
    const AWSError<CoreErrors> skew(CoreErrors::REQUEST_TIME_TOO_SKEWED, RetryableType::RETRYABLE,
                                    "RequestTimeTooSkewed", "");
    ASSERT_TRUE(sdk_attempt(f.strategy, skew, 0));
    ASSERT_EQ(f.observer.events.size(), 1u);
    EXPECT_FALSE(f.observer.events[0].delay.has_value());
}

TEST(ObservingRetryStrategy, ShouldRetryWithoutARecordedDelayReportsNoDelay) {
    // A stale or missing record (no CalculateDelay for this retry count) is
    // not reported as a delay.
    Fixture f;
    (void)f.strategy.CalculateDelayBeforeNextRetry(throttle_error(), 0);
    ASSERT_TRUE(f.strategy.ShouldRetry(throttle_error(), 1));  // different count
    ASSERT_TRUE(f.strategy.ShouldRetry(throttle_error(), 1));  // record already consumed
    ASSERT_EQ(f.observer.events.size(), 2u);
    EXPECT_FALSE(f.observer.events[0].delay.has_value());
    EXPECT_FALSE(f.observer.events[1].delay.has_value());
}

TEST(ObservingRetryStrategy, NullAndSwappedObservers) {
    Fixture f;
    RecordingObserver second;

    f.slot->observer.store(nullptr);
    EXPECT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));  // no observer: still retries
    f.slot->observer.store(&second);
    EXPECT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));
    f.slot->observer.store(&f.observer);
    EXPECT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));

    EXPECT_EQ(second.events.size(), 1u);
    EXPECT_EQ(f.observer.events.size(), 1u);
}

TEST(ObservingRetryStrategy, ThrowingObserverDoesNotChangeTheDecision) {
    Fixture f;
    ThrowingObserver thrower;
    f.slot->observer.store(&thrower);
    EXPECT_TRUE(sdk_attempt(f.strategy, throttle_error(), 0));
    EXPECT_EQ(thrower.calls, 1);
}

// ---- layer 2: AwsDDBClient wires the decorator in rebuild() ------------------

// Nothing listens on 127.0.0.1:1, so every send fails at once with a
// retryable network error and never leaves the host.
constexpr const char* kRefusedEndpoint = "http://127.0.0.1:1";

Aws::Client::ClientConfiguration config_with(const std::shared_ptr<FakeStrategy>& strategy) {
    Aws::Client::ClientConfiguration cfg;
    cfg.retryStrategy = strategy;
    cfg.connectTimeoutMs = 1000;
    cfg.requestTimeoutMs = 1000;
    return cfg;
}

TEST(AwsDDBClientRetryObserver, RebuildWrapsTheCallersStrategy) {
    auto inner = std::make_shared<FakeStrategy>(/*retries_allowed=*/1, /*delay_ms=*/0);
    ddbsh::aws::AwsDDBClient client("us-east-1", kRefusedEndpoint, config_with(inner));
    RecordingObserver observer;
    client.set_retry_observer(&observer);

    EXPECT_FALSE(client.describe_table(ddbsh::DescribeTableIn{"t"}).has_value());

    // The caller's strategy made the decisions (it was wrapped, not replaced):
    // one retry, then it gave up.
    EXPECT_EQ(inner->should_retry_calls, 2);
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(observer.events[0].operation, "DescribeTable");
    EXPECT_EQ(observer.events[0].attempt, 2);
    EXPECT_EQ(observer.events[0].max_attempts, 2);
    EXPECT_FALSE(observer.events[0].throttled);
    ASSERT_TRUE(observer.events[0].delay.has_value());
    EXPECT_EQ(observer.events[0].delay->count(), 0);
}

TEST(AwsDDBClientRetryObserver, RegistrationSurvivesConnectAndClears) {
    auto inner = std::make_shared<FakeStrategy>(/*retries_allowed=*/1, /*delay_ms=*/0);
    ddbsh::aws::AwsDDBClient client("us-east-1", kRefusedEndpoint, config_with(inner));
    RecordingObserver observer;
    client.set_retry_observer(&observer);

    ASSERT_TRUE(client.connect("us-west-2", kRefusedEndpoint).has_value());  // rebuild()
    EXPECT_FALSE(client.list_tables_page(ddbsh::ListTablesPageIn{}).has_value());
    ASSERT_EQ(observer.events.size(), 1u);
    EXPECT_EQ(observer.events[0].operation, "ListTables");

    client.set_retry_observer(nullptr);
    EXPECT_FALSE(client.list_tables_page(ddbsh::ListTablesPageIn{}).has_value());
    EXPECT_EQ(observer.events.size(), 1u);    // cleared: nothing more
    EXPECT_EQ(inner->should_retry_calls, 4);  // the retries still happened
}

// The default-strategy branch of rebuild(), the one the CLI always takes (R-S2-1):
// no retryStrategy in the base configuration, so rebuild() wraps the strategy
// the DynamoDB client's own factory builds. The expected budget is read from
// that same factory (DynamoDBClientConfiguration replaces the core default),
// so the test pins "retry behavior is unchanged" without hard-coding the SDK's
// number. The SDK's real backoff runs, so each case takes a few seconds.
long dynamodb_default_max_attempts() {
    const Aws::DynamoDB::DynamoDBClientConfiguration ddb_cfg(Aws::Client::ClientConfiguration{});
    return ddb_cfg.configFactories.retryStrategyCreateFn()->GetMaxAttempts();
}

void expect_default_strategy_events(const RecordingObserver& observer) {
    const long max_attempts = dynamodb_default_max_attempts();
    ASSERT_GE(max_attempts, 2);  // the default strategy retries at all
    ASSERT_EQ(static_cast<long>(observer.events.size()), max_attempts - 1);
    for (std::size_t i = 0; i < observer.events.size(); ++i) {
        const ddbsh::RetryEvent& event = observer.events[i];
        EXPECT_EQ(event.operation, "DescribeTable");
        EXPECT_EQ(event.attempt, static_cast<int>(i) + 2);
        EXPECT_EQ(event.max_attempts, max_attempts);
        EXPECT_FALSE(event.throttled);
        EXPECT_TRUE(event.delay.has_value()) << "event " << i << " has no delay";
    }
}

TEST(AwsDDBClientRetryObserver, BaseConfigWithoutStrategyWrapsTheDynamoDBDefault) {
    // The CLI's --ca-bundle path: a base configuration with no retryStrategy.
    Aws::Client::ClientConfiguration cfg;
    ASSERT_EQ(cfg.retryStrategy, nullptr);
    cfg.connectTimeoutMs = 1000;
    cfg.requestTimeoutMs = 1000;
    ddbsh::aws::AwsDDBClient client("us-east-1", kRefusedEndpoint, cfg);
    RecordingObserver observer;
    client.set_retry_observer(&observer);

    EXPECT_FALSE(client.describe_table(ddbsh::DescribeTableIn{"t"}).has_value());
    expect_default_strategy_events(observer);
}

TEST(AwsDDBClientRetryObserver, TwoArgumentConstructorWrapsTheDynamoDBDefault) {
    // The CLI's path without --ca-bundle: no base configuration at all.
    ddbsh::aws::AwsDDBClient client("us-east-1", kRefusedEndpoint);
    RecordingObserver observer;
    client.set_retry_observer(&observer);

    EXPECT_FALSE(client.describe_table(ddbsh::DescribeTableIn{"t"}).has_value());
    expect_default_strategy_events(observer);
}

}  // namespace

int main(int argc, char** argv) {
    // Fixed dummy credentials, so the SDK signs from the environment provider
    // and never reaches the instance-metadata or SSO providers. The requests
    // go only to the refused loopback port above.
    setenv("AWS_ACCESS_KEY_ID", "AKIDDBSHTESTONLY0000", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "ddbsh-test-only-not-a-secret", 1);
    unsetenv("AWS_SESSION_TOKEN");
    unsetenv("AWS_PROFILE");
    setenv("AWS_EC2_METADATA_DISABLED", "true", 1);

    ddbsh::aws::AwsApiGuard guard;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
