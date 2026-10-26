// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// connect_reconnect_test.cpp — the CONNECT reconnection seam + prompt-hint
// accessor (AI-2j-3 / D-2j-10 / D-2j-2a).
//
// Pins two parity behaviors the reimplementation was missing (B10/B11):
//   * CONNECT drives IDDBClient::connect(region, endpoint) so every later
//     statement targets the new region/endpoint — the original's reconnect().
//   * Session::connection_hint() reflects the CONNECT live, so a CLI re-reading
//     it before each prompt shows the new region (and the "(*)" override hint).
// Both run fully offline against a recording FakeDDBClient subclass.

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

// A FakeDDBClient that records every connect() call, so the test can assert the
// engine's CONNECT arm reconnects the injected client with the right target.
class RecordingConnectClient : public ddbsh::test::FakeDDBClient {
public:
    ddbsh::Result<void> connect(const std::string& region,
                                const std::string& endpoint) override {
        connects.push_back({region, endpoint});
        return {};
    }
    std::vector<std::pair<std::string, std::string>> connects;
};

ddbsh::Session make_session(ddbsh::IDDBClient& client, const std::string& region,
                            const std::string& endpoint) {
    return ddbsh::Session(ddbsh::SessionConfig{region, endpoint}, client);
}

// ---- the client is reconnected on CONNECT (B11) ----------------------------

TEST(ConnectReconnect, DrivesClientConnectWithRegion) {
    RecordingConnectClient client;
    ddbsh::Session session = make_session(client, "us-east-1", "");

    auto result = session.run("connect us-west-2;");
    ASSERT_TRUE(result.has_value());

    ASSERT_EQ(client.connects.size(), 1U);
    EXPECT_EQ(client.connects[0].first, "us-west-2");
    EXPECT_EQ(client.connects[0].second, "");
}

TEST(ConnectReconnect, DrivesClientConnectWithEndpoint) {
    RecordingConnectClient client;
    ddbsh::Session session = make_session(client, "us-east-1", "");

    auto result = session.run("connect us-west-2 with endpoint \"http://localhost:8000\";");
    ASSERT_TRUE(result.has_value());

    ASSERT_EQ(client.connects.size(), 1U);
    EXPECT_EQ(client.connects[0].first, "us-west-2");
    EXPECT_EQ(client.connects[0].second, "http://localhost:8000");
}

// ---- connection_hint reflects the CONNECT live (B10) -----------------------

TEST(ConnectReconnect, HintStartsAtStartupConfig) {
    RecordingConnectClient client;
    ddbsh::Session session = make_session(client, "eu-west-1", "");

    const ddbsh::ConnectionHint hint = session.connection_hint();
    EXPECT_EQ(hint.region, "eu-west-1");
    EXPECT_FALSE(hint.endpoint_overridden);
}

TEST(ConnectReconnect, HintTracksRegionAfterConnect) {
    RecordingConnectClient client;
    ddbsh::Session session = make_session(client, "us-east-1", "");

    ASSERT_TRUE(session.run("connect ap-south-1;").has_value());
    const ddbsh::ConnectionHint hint = session.connection_hint();
    EXPECT_EQ(hint.region, "ap-south-1");
    EXPECT_FALSE(hint.endpoint_overridden);
}

TEST(ConnectReconnect, HintTracksEndpointOverrideAfterConnect) {
    RecordingConnectClient client;
    ddbsh::Session session = make_session(client, "us-east-1", "");

    ASSERT_TRUE(session.run("connect us-west-2 with endpoint \"http://localhost:8000\";")
                    .has_value());
    const ddbsh::ConnectionHint hint = session.connection_hint();
    EXPECT_EQ(hint.region, "us-west-2");
    EXPECT_TRUE(hint.endpoint_overridden);
}

TEST(ConnectReconnect, HintClearsEndpointOverrideOnPlainConnect) {
    RecordingConnectClient client;
    ddbsh::Session session = make_session(client, "us-east-1", "http://localhost:8000");

    EXPECT_TRUE(session.connection_hint().endpoint_overridden);
    ASSERT_TRUE(session.run("connect us-west-2;").has_value());
    const ddbsh::ConnectionHint hint = session.connection_hint();
    EXPECT_EQ(hint.region, "us-west-2");
    EXPECT_FALSE(hint.endpoint_overridden);
}

}  // namespace
