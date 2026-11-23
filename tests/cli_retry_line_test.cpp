// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// cli_retry_line_test.cpp — the CLI's `--show-retries` line (P12, D-P12-20).
//
// format_retry_line is CLI presentation policy (cli/repl.cpp); these tests pin
// its exact text for each optional field, and RetryLineWriter's write to the
// stream it was given (a temporary file standing in for stderr).

#include <chrono>
#include <cstdio>
#include <string>

#include <gtest/gtest.h>

#include "ddbsh/retry_event.hpp"
#include "repl.hpp"

namespace {

using ddbsh::RetryEvent;
using ddbsh::cli::format_retry_line;

RetryEvent throttle_event() {
    RetryEvent e;
    e.operation = "Scan";
    e.attempt = 2;
    e.max_attempts = 3;
    e.error_code = "ProvisionedThroughputExceededException";
    e.throttled = true;
    e.delay = std::chrono::milliseconds{150};
    return e;
}

TEST(CliRetryLine, ThrottledWithEveryField) {
    EXPECT_EQ(format_retry_line(throttle_event()),
              "ddbsh: Scan throttled (ProvisionedThroughputExceededException); attempt 2 of 3 "
              "in 150 ms\n");
}

TEST(CliRetryLine, NonThrottleSaysFailed) {
    RetryEvent e = throttle_event();
    e.operation = "Query";
    e.error_code = "InternalServerError";
    e.throttled = false;
    e.attempt = 3;
    e.delay = std::chrono::milliseconds{40};
    EXPECT_EQ(format_retry_line(e),
              "ddbsh: Query failed (InternalServerError); attempt 3 of 3 in 40 ms\n");
}

TEST(CliRetryLine, OptionalFieldsAreOmittedWhenUnknown) {
    RetryEvent e;
    e.attempt = 2;  // no operation, no code, no budget, no delay
    EXPECT_EQ(format_retry_line(e), "ddbsh: request failed; attempt 2\n");
}

TEST(CliRetryLine, ZeroDelayIsShown) {
    RetryEvent e = throttle_event();
    e.delay = std::chrono::milliseconds{0};
    EXPECT_EQ(format_retry_line(e),
              "ddbsh: Scan throttled (ProvisionedThroughputExceededException); attempt 2 of 3 "
              "in 0 ms\n");
}

TEST(CliRetryLine, WriterWritesTheLineToItsStream) {
    std::FILE* err = std::tmpfile();
    ASSERT_NE(err, nullptr);
    ddbsh::cli::RetryLineWriter writer(err);
    writer.on_retry(throttle_event());

    std::rewind(err);
    std::string written;
    char buf[256];
    while (std::fgets(buf, sizeof buf, err) != nullptr) {
        written += buf;
    }
    std::fclose(err);
    EXPECT_EQ(written, format_retry_line(throttle_event()));
}

}  // namespace
