// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// walking_path_test.cpp — proves the end-to-end engine seam with no AWS account.
//
// text -> Session::parse -> Statement -> Session::execute (via FakeDDBClient)
// -> ExecOutcome -> Renderer -> bytes. This is the §14 walking skeleton's
// fidelity signal: it needs no backend and runs fast.

#include <string>

#include <gtest/gtest.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/version.hpp"

#include "fake_ddb_client.hpp"

namespace {

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

TEST(WalkingPath, VersionIsSemantic) {
    // D15: the reimplementation version starts at 0.9.1; bumped per merge. The
    // exact value lives in CMake project(VERSION ...); assert it is present and
    // dotted rather than hard-coding a literal that rots on every patch bump.
    const std::string v = ddbsh::version();
    EXPECT_FALSE(v.empty());
    EXPECT_NE(v.find('.'), std::string::npos);
}

TEST(WalkingPath, ShowVersionRendersBanner) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    // Derive the expected banner from the project version so this does not rot
    // when the developer bumps the patch per merge (shared.md Versioning).
    const std::string expected = "ddbsh - version " + ddbsh::version() + "\n";
    auto outcome = session.run("show version;");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome), expected);
}

TEST(WalkingPath, ShowVersionIsCaseInsensitive) {
    // arch §4: keywords are case-insensitive.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    const std::string expected = "ddbsh - version " + ddbsh::version() + "\n";
    auto outcome = session.run("SHOW VERSION;");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome), expected);
}

TEST(WalkingPath, QuitRendersStatusToken) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    auto outcome = session.run("quit;");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome), "QUIT\n");
}

TEST(WalkingPath, EmptyInputRendersNothing) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    auto outcome = session.run("");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome), "");
}

TEST(WalkingPath, GarbageInputIsAParseError) {
    // The failure path is a Result error, never an exception or exit (D12).
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);

    auto outcome = session.run("frobnicate the widget;");
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ddbsh::Errc::parse_error);
}

}  // namespace
