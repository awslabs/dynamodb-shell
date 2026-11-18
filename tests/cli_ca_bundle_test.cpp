// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// cli_ca_bundle_test.cpp — the CLI CA-bundle resolution helper
// (ddbsh::cli::resolve_ca_bundle, cli/repl.{hpp,cpp}), exercised offline.
//
// D1 (P7): the native CLI gains AWS-CLI-parity TLS trust ergonomics — a
// --ca-bundle flag and the AWS_CA_BUNDLE environment variable, mapped onto
// ClientConfiguration.caFile. The precedence logic is pure (no SDK, no
// terminal), so it lives in the cli-core unit and is unit-tested here. These
// tests pin: flag resolves, env resolves, the flag wins over env, and empty /
// unset inputs do not count as "set".

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "repl.hpp"

namespace {

using ddbsh::cli::resolve_ca_bundle;

TEST(CliCaBundle, NeitherSetYieldsNullopt) {
    EXPECT_FALSE(resolve_ca_bundle(std::nullopt, nullptr).has_value());
}

TEST(CliCaBundle, EmptyEnvStringDoesNotCount) {
    EXPECT_FALSE(resolve_ca_bundle(std::nullopt, "").has_value());
}

TEST(CliCaBundle, EnvOnlyResolves) {
    const auto r = resolve_ca_bundle(std::nullopt, "/etc/pki/env-bundle.pem");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/etc/pki/env-bundle.pem");
}

TEST(CliCaBundle, FlagOnlyResolves) {
    const auto r = resolve_ca_bundle(std::optional<std::string>{"/home/u/flag.pem"}, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/home/u/flag.pem");
}

TEST(CliCaBundle, FlagWinsOverEnv) {
    const auto r = resolve_ca_bundle(std::optional<std::string>{"/home/u/flag.pem"},
                                     "/etc/pki/env-bundle.pem");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/home/u/flag.pem");
}

TEST(CliCaBundle, EmptyFlagStringFallsBackToEnv) {
    // An empty flag value does not count as "set", so the env wins.
    const auto r = resolve_ca_bundle(std::optional<std::string>{""}, "/etc/pki/env-bundle.pem");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/etc/pki/env-bundle.pem");
}

TEST(CliCaBundle, EmptyFlagAndNoEnvYieldsNullopt) {
    EXPECT_FALSE(resolve_ca_bundle(std::optional<std::string>{""}, nullptr).has_value());
}

}  // namespace
