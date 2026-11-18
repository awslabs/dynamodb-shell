// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// cli_ca_bundle_test.cpp — the CLI CA-bundle resolution helper
// (ddbsh::cli::resolve_ca_bundle, cli/repl.{hpp,cpp}), exercised offline.
//
// D1 (P7): the native CLI gains AWS-CLI-parity TLS trust ergonomics — a
// --ca-bundle flag and the AWS_CA_BUNDLE environment variable, mapped onto
// ClientConfiguration.caFile. P8 (D-P8-2) adds a third, lowest-precedence
// source: the resolved profile's `ca_bundle` config setting. The precedence
// logic is pure (no SDK, no terminal), so it lives in the cli-core unit and is
// unit-tested here. These tests pin the full precedence — flag > env > profile
// > none — and that empty / unset inputs from any source do not count as "set".

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "repl.hpp"

namespace {

using ddbsh::cli::resolve_ca_bundle;

TEST(CliCaBundle, NeitherSetYieldsNullopt) {
    EXPECT_FALSE(resolve_ca_bundle(std::nullopt, nullptr, std::nullopt).has_value());
}

TEST(CliCaBundle, EmptyEnvStringDoesNotCount) {
    EXPECT_FALSE(resolve_ca_bundle(std::nullopt, "", std::nullopt).has_value());
}

TEST(CliCaBundle, EnvOnlyResolves) {
    const auto r = resolve_ca_bundle(std::nullopt, "/etc/pki/env-bundle.pem", std::nullopt);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/etc/pki/env-bundle.pem");
}

TEST(CliCaBundle, FlagOnlyResolves) {
    const auto r =
        resolve_ca_bundle(std::optional<std::string>{"/home/u/flag.pem"}, nullptr, std::nullopt);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/home/u/flag.pem");
}

TEST(CliCaBundle, FlagWinsOverEnv) {
    const auto r = resolve_ca_bundle(std::optional<std::string>{"/home/u/flag.pem"},
                                     "/etc/pki/env-bundle.pem", std::nullopt);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/home/u/flag.pem");
}

TEST(CliCaBundle, EmptyFlagStringFallsBackToEnv) {
    // An empty flag value does not count as "set", so the env wins.
    const auto r =
        resolve_ca_bundle(std::optional<std::string>{""}, "/etc/pki/env-bundle.pem", std::nullopt);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/etc/pki/env-bundle.pem");
}

TEST(CliCaBundle, EmptyFlagAndNoEnvYieldsNullopt) {
    EXPECT_FALSE(
        resolve_ca_bundle(std::optional<std::string>{""}, nullptr, std::nullopt).has_value());
}

// --- P8: the profile (third / lowest) source --------------------------------

TEST(CliCaBundle, ProfileOnlyResolves) {
    const auto r =
        resolve_ca_bundle(std::nullopt, nullptr, std::optional<std::string>{"/home/u/profile.pem"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/home/u/profile.pem");
}

TEST(CliCaBundle, EmptyProfileStringDoesNotCount) {
    EXPECT_FALSE(
        resolve_ca_bundle(std::nullopt, nullptr, std::optional<std::string>{""}).has_value());
}

TEST(CliCaBundle, EnvWinsOverProfile) {
    const auto r = resolve_ca_bundle(std::nullopt, "/etc/pki/env-bundle.pem",
                                     std::optional<std::string>{"/home/u/profile.pem"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/etc/pki/env-bundle.pem");
}

TEST(CliCaBundle, FlagWinsOverProfile) {
    const auto r = resolve_ca_bundle(std::optional<std::string>{"/home/u/flag.pem"}, nullptr,
                                     std::optional<std::string>{"/home/u/profile.pem"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/home/u/flag.pem");
}

TEST(CliCaBundle, FlagWinsOverEnvAndProfile) {
    const auto r =
        resolve_ca_bundle(std::optional<std::string>{"/home/u/flag.pem"}, "/etc/pki/env-bundle.pem",
                          std::optional<std::string>{"/home/u/profile.pem"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/home/u/flag.pem");
}

TEST(CliCaBundle, EmptyFlagAndEnvFallsBackToProfile) {
    const auto r = resolve_ca_bundle(std::optional<std::string>{""}, "",
                                     std::optional<std::string>{"/home/u/profile.pem"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/home/u/profile.pem");
}

}  // namespace
