// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_client_retry_test.cpp — WireClient retryable-error resilience (HT-1).
//
// The authoritative regression guard for the HT-1 fix: a retryable DynamoDB
// service error (a `__type` document whose exception name is transient) is
// retried with bounded exponential backoff + full jitter, below the IDDBClient
// seam, reproducing the resilience the SDK-backed CLI got for free from its
// DefaultRetryStrategy. Real DynamoDB cannot be made to throttle on demand, so
// these offline tests — not the real-DDB burst smoke — are the real guard.
//
// A TestWireClient subclass overrides backoff_sleep (no-op: no real sleep in
// CI), jitter_unit (deterministic timing), and lowers the attempt cap so
// exhaustion is reached quickly. The CannedTransport scripts the `__type`
// bodies and records every send() so the attempt count is asserted exactly.

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/testing/canned_transport.hpp"
#include "ddbsh/value.hpp"
#include "ddbsh/wire/codec.hpp"
#include "ddbsh/wire/wire_client.hpp"

namespace {

using ddbsh::Errc;
using ddbsh::testing::CannedTransport;
using ddbsh::wire::WireClient;

// A throttle error document (retryable). The exception name carries the usual
// shape#Name prefix so the test also exercises the '#'-stripping.
constexpr const char* kThrottleBody =
    R"({"__type":"com.amazonaws.dynamodb#ThrottlingException","message":"Rate exceeded"})";
// A validation error document (NOT retryable).
constexpr const char* kValidationBody =
    R"({"__type":"com.amazonaws.dynamodb#ValidationException","message":"bad input"})";
// A conditional-check-failed document (terminal contract outcome, NOT retryable).
constexpr const char* kCcfBody =
    R"({"__type":"x#ConditionalCheckFailedException","message":"The conditional request failed"})";
// A successful DescribeTable body.
constexpr const char* kDescribeOkBody = R"({"Table":{"TableName":"t","TableStatus":"ACTIVE"}})";

// A WireClient whose sleep is a no-op (records the requested intervals), whose
// jitter is a fixed fraction, and whose attempt cap a test can lower.
class TestWireClient : public WireClient {
public:
    explicit TestWireClient(ddbsh::wire::Transport& t, unsigned attempt_cap = 11,
                            double jitter_fraction = 0.0)
        : WireClient(t), jitter_(jitter_fraction) {
        max_retry_attempts_ = attempt_cap;
    }

    std::vector<long long> sleeps_ms;  // intervals passed to backoff_sleep

protected:
    void backoff_sleep(std::chrono::milliseconds interval) override {
        sleeps_ms.push_back(interval.count());
        // No real sleep: the retry loop runs instantly in CI.
    }
    double jitter_unit() override { return jitter_; }

private:
    double jitter_;
};

ddbsh::DescribeTableIn describe_t() {
    return ddbsh::DescribeTableIn{"t"};
}

ddbsh::UpdateItemIn update_t() {
    ddbsh::UpdateItemIn in;
    in.table_name = "t";
    in.key = {{"id", ddbsh::Value::string_value("k1")}};
    in.update_expression = "SET #aaaa1 = :vaaa1";
    in.expression_attribute_names = {{"#aaaa1", "v"}};
    in.expression_attribute_values = {{":vaaa1", ddbsh::Value::number_value("2")}};
    return in;
}

// ---- §6.1 retry behavior ---------------------------------------------------

TEST(WireClientRetry, RetriesThrottleThenSucceeds) {
    CannedTransport transport;
    transport.push_response(kThrottleBody);    // first attempt: throttled
    transport.push_response(kDescribeOkBody);  // retry: succeeds
    TestWireClient client(transport);

    auto out = client.describe_table(describe_t());
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->table_name, "t");
    EXPECT_EQ(transport.calls().size(), 2u);  // one retry fired
    EXPECT_TRUE(transport.drained());
}

TEST(WireClientRetry, RetryExhaustionReturnsMappedError) {
    CannedTransport transport;
    constexpr unsigned kCap = 3;
    for (unsigned i = 0; i < kCap; ++i) {
        transport.push_response(kThrottleBody);
    }
    TestWireClient client(transport, kCap);

    auto out = client.describe_table(describe_t());
    ASSERT_FALSE(out.has_value());
    // On exhaustion the LAST throttle body is parsed to the exact byte-identical
    // error the one-shot path would have produced (not a synthesized error).
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "Rate exceeded");
    ASSERT_TRUE(out.error().service_error_name.has_value());
    EXPECT_EQ(*out.error().service_error_name, "ThrottlingException");
    EXPECT_EQ(transport.calls().size(), kCap);  // bounded: exactly the cap, no infinite loop
    EXPECT_TRUE(transport.drained());
}

TEST(WireClientRetry, NonRetryableNotRetried) {
    CannedTransport transport;
    transport.push_response(kValidationBody);
    transport.push_response(kDescribeOkBody);  // must NOT be consumed
    TestWireClient client(transport);

    auto out = client.describe_table(describe_t());
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "bad input");
    EXPECT_EQ(transport.calls().size(), 1u);  // single attempt, no retry
    EXPECT_FALSE(transport.drained());        // the success reply was left untouched
}

TEST(WireClientRetry, ConditionalCheckFailedNotRetried) {
    CannedTransport transport;
    transport.push_response(kCcfBody);
    transport.push_response(R"({"Attributes":{"v":{"N":"2"}}})");  // must NOT be consumed
    TestWireClient client(transport);

    auto out = client.update_item(update_t());
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::conditional_check_failed);
    EXPECT_EQ(out.error().message, "The conditional request failed");
    EXPECT_EQ(transport.calls().size(), 1u);
    EXPECT_FALSE(transport.drained());
}

TEST(WireClientRetry, TransportFailureNotRetried) {
    CannedTransport transport;
    transport.push_error(
        ddbsh::Error{Errc::service_error, "transport send failed", std::nullopt, std::nullopt});
    transport.push_response(kDescribeOkBody);  // must NOT be consumed
    TestWireClient client(transport);

    // An opaque transport-level failure has no `__type` to classify, so it is
    // terminal (D-HT1-3): a single attempt, the error surfaced unchanged.
    auto out = client.describe_table(describe_t());
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
    EXPECT_EQ(out.error().message, "transport send failed");
    EXPECT_EQ(transport.calls().size(), 1u);
    EXPECT_FALSE(transport.drained());
}

TEST(WireClientRetry, BackoffIsBoundedAndGrowsToCap) {
    CannedTransport transport;
    constexpr unsigned kCap = 8;
    for (unsigned i = 0; i < kCap; ++i) {
        transport.push_response(kThrottleBody);
    }
    // Full jitter at fraction 1.0 == the ceiling, so the recorded intervals are
    // the undiluted exponential-with-cap schedule: 50,100,200,400,800,1000,1000.
    TestWireClient client(transport, kCap, /*jitter_fraction=*/1.0);

    auto out = client.describe_table(describe_t());
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(transport.calls().size(), kCap);
    ASSERT_EQ(client.sleeps_ms.size(), kCap - 1);  // one sleep between each attempt

    // Non-decreasing, and the tail saturates at the 1000ms cap.
    for (std::size_t i = 1; i < client.sleeps_ms.size(); ++i) {
        EXPECT_GE(client.sleeps_ms[i], client.sleeps_ms[i - 1]) << "interval " << i;
    }
    EXPECT_EQ(client.sleeps_ms.front(), 50);   // base
    EXPECT_EQ(client.sleeps_ms.back(), 1000);  // capped
    for (long long ms : client.sleeps_ms) {
        EXPECT_LE(ms, 1000) << "no interval exceeds the cap";
    }
}

// ---- §6.1(7) error_type_name codec helper ----------------------------------

TEST(WireErrorTypeName, ExtractsBareNameFromTypeDocument) {
    auto name = ddbsh::wire::error_type_name(kThrottleBody);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "ThrottlingException");
}

TEST(WireErrorTypeName, TypeDocumentWithoutHashReturnsWholeName) {
    auto name = ddbsh::wire::error_type_name(R"({"__type":"ServiceUnavailable"})");
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "ServiceUnavailable");
}

TEST(WireErrorTypeName, SuccessBodyReturnsNullopt) {
    EXPECT_FALSE(ddbsh::wire::error_type_name(kDescribeOkBody).has_value());
    EXPECT_FALSE(ddbsh::wire::error_type_name(R"({})").has_value());
}

TEST(WireErrorTypeName, MalformedBodyReturnsNullopt) {
    EXPECT_FALSE(ddbsh::wire::error_type_name("not json at all").has_value());
    EXPECT_FALSE(ddbsh::wire::error_type_name("[1,2,3]").has_value());           // not an object
    EXPECT_FALSE(ddbsh::wire::error_type_name(R"({"__type":42})").has_value());  // not a string
}

}  // namespace
