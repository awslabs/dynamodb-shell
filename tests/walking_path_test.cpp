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

TEST(WalkingPath, VersionIs095) {
    // D15: the reimplementation version starts at 0.9.1; bumped per merge.
    EXPECT_EQ(ddbsh::version(), "0.9.5");
}

TEST(WalkingPath, ShowVersionRendersBanner) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    auto outcome = session.run("show version;");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome), "ddbsh - version 0.9.5\n");
}

TEST(WalkingPath, ShowVersionIsCaseInsensitive) {
    // arch §4: keywords are case-insensitive.
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    ddbsh::render::Renderer renderer;

    auto outcome = session.run("SHOW VERSION;");
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
    EXPECT_EQ(renderer.render(*outcome), "ddbsh - version 0.9.5\n");
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
