// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// reentrancy_test.cpp — proves N-threads / N-sessions reentrancy (D3, amendment 7).
//
// Constructs many Sessions across many threads and runs overlapping statements
// concurrently. Because the scanner is reentrant (%option reentrant) and the
// parser is a per-parse object with no global state (P2a-02 §7), each Session
// parses independently. A shared FakeDDBClient is used to prove the client
// seam can be shared safely across sessions (the AWS SDK client is thread-safe;
// the fake holds no mutable state).

#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

TEST(Reentrancy, ManySessionsAcrossThreads) {
    constexpr int kThreads = 16;
    constexpr int kIterations = 200;

    ddbsh::test::FakeDDBClient shared_client;  // shared across all sessions

    std::vector<std::thread> threads;
    std::vector<int> failures(kThreads, 0);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            ddbsh::render::Renderer renderer;
            for (int i = 0; i < kIterations; ++i) {
                // Each thread constructs its OWN Session (N threads / N
                // sessions). Sharing one Session across threads is explicitly
                // NOT the contract.
                ddbsh::Session session(ddbsh::SessionConfig{"us-east-1", ""}, shared_client);
                auto outcome = session.run("show version;");
                if (!outcome.has_value() ||
                    renderer.render(*outcome) != "ddbsh - version 0.9.4\n") {
                    ++failures[t];
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(failures[t], 0) << "thread " << t << " observed parse/render interference";
    }
}

}  // namespace
