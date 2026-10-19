// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// cli_exit_code_test.cpp — the CLI isql-assembly driver (cli/repl.{hpp,cpp}),
// exercised offline against a FakeDDBClient-backed Session. Closes the carried
// "isql-assembly lacks a focused subprocess test" debt and pins the mixed-batch
// exit-code semantics (2h-3).
//
// Exit-code contract (matching the original run loop, arch §3): the process
// exit code is the LAST EXECUTED statement's run() status across the whole
// source — a later success RESETS an earlier failure to 0. A PARSE error never
// contributes (the oracle's `retval` is updated only by pCommand->run()). With
// -e (abort_on_error), execution stops at the first failing statement, so that
// failure is the last executed and the exit code is nonzero.
//
// Offline building blocks:
//   * fail path   — INSERT into an unregistered table => Errc::no_such_table at
//                   run() time (a command failure, not a parse error).
//   * success path— DROP TABLE IF EXISTS <missing> => silent success (no API
//                   call, no output).
// Both are deterministic and need no AWS, so the driver runs entirely offline.

#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"
#include "repl.hpp"

namespace {

using ddbsh::cli::run_source;
using ddbsh::cli::StreamLineSource;
using ddbsh::cli::Terminator;

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

// Run a whole source string through the driver and return the exit code. No
// table is registered, so `insert into nope ...` fails (no_such_table) and
// `drop table if exists ...` succeeds silently.
int run(const std::string& source, bool abort_on_error) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;
    std::istringstream in(source);
    StreamLineSource src(in);
    Terminator term;  // default `go`; a single-batch string flushes at EOF
    return run_source(session, renderer, src, term, abort_on_error);
}

constexpr const char* kFail = "insert into nope ( id ) values ( 1 );";
constexpr const char* kOk = "drop table if exists gone;";

// ---- last-executed-statement semantics (no -e) -----------------------------

TEST(CliExitCode, FailThenSucceedExitsZero) {
    // A later success resets the earlier failure (the headline 2h-3 case).
    EXPECT_EQ(run(std::string(kFail) + "\n" + kOk + "\n", /*abort_on_error=*/false), 0);
}

TEST(CliExitCode, SucceedThenFailExitsNonZero) {
    EXPECT_EQ(run(std::string(kOk) + "\n" + kFail + "\n", /*abort_on_error=*/false), 1);
}

TEST(CliExitCode, SingleFailureExitsNonZero) {
    EXPECT_EQ(run(std::string(kFail) + "\n", /*abort_on_error=*/false), 1);
}

TEST(CliExitCode, SingleSuccessExitsZero) {
    EXPECT_EQ(run(std::string(kOk) + "\n", /*abort_on_error=*/false), 0);
}

// ---- abort-on-error (-e) ---------------------------------------------------

TEST(CliExitCode, DashEFailThenSucceedAbortsAtFailureExitsNonZero) {
    // -e stops at the first failure, so it is the last executed => nonzero.
    EXPECT_EQ(run(std::string(kFail) + "\n" + kOk + "\n", /*abort_on_error=*/true), 1);
}

TEST(CliExitCode, DashESucceedThenFailExitsNonZero) {
    EXPECT_EQ(run(std::string(kOk) + "\n" + kFail + "\n", /*abort_on_error=*/true), 1);
}

TEST(CliExitCode, DashEAllSucceedExitsZero) {
    EXPECT_EQ(run(std::string(kOk) + "\n" + kOk + "\n", /*abort_on_error=*/true), 0);
}

// ---- parse errors never contribute to the exit code ------------------------

TEST(CliExitCode, PureParseErrorExitsZero) {
    EXPECT_EQ(run("this is not valid sql;\n", /*abort_on_error=*/false), 0);
}

TEST(CliExitCode, FailureThenTrailingParseErrorExitsNonZero) {
    // The run() failure sets the exit code; the trailing parse error leaves it
    // untouched (does not reset it to 0).
    EXPECT_EQ(run(std::string(kFail) + " @@@garbage@@@\n", /*abort_on_error=*/false), 1);
}

TEST(CliExitCode, SuccessThenTrailingParseErrorExitsZero) {
    EXPECT_EQ(run(std::string(kOk) + " @@@garbage@@@\n", /*abort_on_error=*/false), 0);
}

// ---- QUIT counts as a successful last-executed statement -------------------

TEST(CliExitCode, FailThenQuitExitsZero) {
    EXPECT_EQ(run(std::string(kFail) + "\nquit;\n", /*abort_on_error=*/false), 0);
}

// ---- exit code spans batches (the original's single run loop) --------------

TEST(CliExitCode, FailInFirstBatchSucceedInSecondExitsZero) {
    // Two `go`-separated batches: the failure in batch 1 is reset by the
    // success in batch 2, because the exit code tracks the last executed
    // statement across the whole source, not per batch.
    const std::string source = std::string(kFail) + "\ngo\n" + kOk + "\n";
    EXPECT_EQ(run(source, /*abort_on_error=*/false), 0);
}

}  // namespace
