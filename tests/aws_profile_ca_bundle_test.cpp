// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// aws_profile_ca_bundle_test.cpp — the AWS-adapter profile `ca_bundle` reader
// (ddbsh::aws::profile_ca_bundle, aws/aws_ddb_client.cpp), exercised offline
// against an isolated temp AWS config file.
//
// P8 (D-P8-1/3/5): the AWS SDK for C++ (1.11.916) does NOT populate
// ClientConfiguration.caFile from a profile's `ca_bundle` key, so ddbsh reads
// it explicitly via Aws::Config::GetCachedConfigProfile(profile).GetValue(
// "ca_bundle"). This test pins that read against a temp ~/.aws/config: the
// resolved profile's `ca_bundle` is returned; an absent key yields nullopt; the
// resolved profile follows explicit-profile / AWS_PROFILE / default.
//
// The SDK config cache loads once, so this binary provides its own main(): it
// writes the temp config and points AWS_CONFIG_FILE at it BEFORE Aws::InitAPI
// runs (D-P8-5), wraps the whole process in one AwsApiGuard, and links
// GTest::gtest (not gtest_main). It makes NO network call — the read is a pure
// config-file parse. The real ~/.aws is never touched.

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <aws/core/config/ConfigAndCredentialsCacheManager.h>

#include "ddbsh/aws/aws_api_guard.hpp"
#include "ddbsh/aws/aws_ddb_client.hpp"

namespace {

using ddbsh::aws::profile_ca_bundle;

// The config file the whole process reads (written by main() before InitAPI).
// Named profiles use `[profile <name>]`; the default profile uses `[default]`.
// `withca` carries a ca_bundle; `noca` does not; `[default]` carries its own.
constexpr const char* kWithCaPath = "/tmp/ddbsh-test-withca-bundle.pem";
constexpr const char* kDefaultCaPath = "/tmp/ddbsh-test-default-bundle.pem";

TEST(AwsProfileCaBundle, ExplicitProfileWithKeyResolves) {
    const auto r = profile_ca_bundle(std::optional<std::string>{"withca"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, kWithCaPath);
}

TEST(AwsProfileCaBundle, ExplicitProfileWithoutKeyYieldsNullopt) {
    EXPECT_FALSE(profile_ca_bundle(std::optional<std::string>{"noca"}).has_value());
}

TEST(AwsProfileCaBundle, MissingProfileYieldsNullopt) {
    EXPECT_FALSE(profile_ca_bundle(std::optional<std::string>{"does-not-exist"}).has_value());
}

TEST(AwsProfileCaBundle, EmptyExplicitProfileUsesAwsProfileEnv) {
    // With no explicit profile, AWS_PROFILE selects the profile.
    setenv("AWS_PROFILE", "withca", 1);
    unsetenv("AWS_DEFAULT_PROFILE");
    const auto r = profile_ca_bundle(std::nullopt);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, kWithCaPath);
    unsetenv("AWS_PROFILE");
}

TEST(AwsProfileCaBundle, FallsBackToDefaultProfile) {
    // No explicit profile and no AWS_PROFILE / AWS_DEFAULT_PROFILE => "default".
    unsetenv("AWS_PROFILE");
    unsetenv("AWS_DEFAULT_PROFILE");
    const auto r = profile_ca_bundle(std::nullopt);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, kDefaultCaPath);
}

TEST(AwsProfileCaBundle, ExplicitProfileWinsOverAwsProfileEnv) {
    // An explicit profile is honored even when AWS_PROFILE names another.
    setenv("AWS_PROFILE", "noca", 1);
    const auto r = profile_ca_bundle(std::optional<std::string>{"withca"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, kWithCaPath);
    unsetenv("AWS_PROFILE");
}

}  // namespace

int main(int argc, char** argv) {
    // Write an isolated AWS config file and point the SDK at it BEFORE InitAPI,
    // so the config cache loads from the temp file, not the real ~/.aws (D-P8-5).
    const std::string cfg_path = "/tmp/ddbsh-test-aws-config-profile-ca-bundle.ini";
    {
        std::ofstream cfg(cfg_path);
        cfg << "[default]\n"
            << "ca_bundle = " << kDefaultCaPath << "\n"
            << "\n"
            << "[profile withca]\n"
            << "region = us-east-1\n"
            << "ca_bundle = " << kWithCaPath << "\n"
            << "\n"
            << "[profile noca]\n"
            << "region = us-east-1\n";
    }
    setenv("AWS_CONFIG_FILE", cfg_path.c_str(), 1);
    // Start clean: no profile env so the default-fallback test is deterministic
    // regardless of the caller's environment.
    unsetenv("AWS_PROFILE");
    unsetenv("AWS_DEFAULT_PROFILE");

    ddbsh::aws::AwsApiGuard guard;
    // Belt and suspenders: force a load from the now-set AWS_CONFIG_FILE in case
    // the cache was warmed earlier in the process (D-P8-5).
    Aws::Config::ReloadCachedConfigFile();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
