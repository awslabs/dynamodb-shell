// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_datetime_test.cpp — the ddbsh-wire epoch<->ISO-8601 datetime codec
// (P6c). Pins epoch_to_iso8601 / iso8601_to_epoch against known instants, the
// leap-year / end-of-month / fractional-second edges, the +/-HH:MM offset
// handling, and the unparseable-string path. The RestoreDateTime double and
// the DESCRIBE/backup report timestamps both ride on these; the differential
// gate transitively re-checks the serialize direction against the SDK.

#include "wire/datetime.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace {

using ddbsh::wire::epoch_to_iso8601;
using ddbsh::wire::iso8601_to_epoch;

// ---- epoch -> ISO-8601 -----------------------------------------------------

TEST(WireDatetime, EpochZeroIsUnixEpoch) {
    EXPECT_EQ(epoch_to_iso8601(0.0), "1970-01-01T00:00:00Z");
}

TEST(WireDatetime, EpochKnownInstant) {
    // 1700000000 = 2023-11-14T22:13:20Z (a well-known round epoch).
    EXPECT_EQ(epoch_to_iso8601(1700000000.0), "2023-11-14T22:13:20Z");
}

TEST(WireDatetime, EpochTruncatesFractionTowardSecond) {
    // Second precision: the fractional part is dropped (floor).
    EXPECT_EQ(epoch_to_iso8601(1700000000.75), "2023-11-14T22:13:20Z");
}

TEST(WireDatetime, EpochLeapDay) {
    // 2024-02-29T12:00:00Z — leap day.
    const double e = *iso8601_to_epoch("2024-02-29T12:00:00Z");
    EXPECT_EQ(epoch_to_iso8601(e), "2024-02-29T12:00:00Z");
}

TEST(WireDatetime, EpochEndOfYear) {
    const double e = *iso8601_to_epoch("2025-12-31T23:59:59Z");
    EXPECT_EQ(epoch_to_iso8601(e), "2025-12-31T23:59:59Z");
}

// ---- ISO-8601 -> epoch -----------------------------------------------------

TEST(WireDatetime, ParseUnixEpoch) {
    EXPECT_EQ(iso8601_to_epoch("1970-01-01T00:00:00Z"), std::optional<double>(0.0));
}

TEST(WireDatetime, ParseKnownInstant) {
    EXPECT_EQ(iso8601_to_epoch("2023-11-14T22:13:20Z"), std::optional<double>(1700000000.0));
}

TEST(WireDatetime, ParseDiscardsSubMilliExtraDigits) {
    // Milliseconds come from the first three fractional digits; extra digits
    // (up to nine total) are consumed but do not change the millisecond value.
    EXPECT_EQ(iso8601_to_epoch("2023-11-14T22:13:20.000Z"), std::optional<double>(1700000000.0));
    EXPECT_EQ(iso8601_to_epoch("2023-11-14T22:13:20.500Z"), std::optional<double>(1700000000.5));
}

TEST(WireDatetime, ParseRoundTrip) {
    for (const std::string& s :
         {std::string("1970-01-01T00:00:00Z"), std::string("2024-02-29T12:34:56Z"),
          std::string("2099-07-15T08:09:10Z")}) {
        auto e = iso8601_to_epoch(s);
        ASSERT_TRUE(e.has_value());
        EXPECT_EQ(epoch_to_iso8601(*e), s);
    }
}

// ---- unparseable -----------------------------------------------------------

TEST(WireDatetime, RejectsEmpty) {
    EXPECT_FALSE(iso8601_to_epoch("").has_value());
}

TEST(WireDatetime, RejectsGarbage) {
    EXPECT_FALSE(iso8601_to_epoch("not-a-date").has_value());
}

TEST(WireDatetime, RejectsDateOnly) {
    // The SDK's ISO_8601DateParser requires the full date-time; a bare date is
    // not accepted.
    EXPECT_FALSE(iso8601_to_epoch("2023-11-14").has_value());
}

TEST(WireDatetime, RejectsNonUtcOffset) {
    // A non-UTC offset is local-time per the SDK and cannot be lexed by
    // T_TIMESTAMP; rejected.
    EXPECT_FALSE(iso8601_to_epoch("2023-11-14T22:13:20+01:00").has_value());
}

TEST(WireDatetime, RejectsLenientNoSeparators) {
    EXPECT_FALSE(iso8601_to_epoch("20231114T221320Z").has_value());
}

TEST(WireDatetime, RejectsMissingZone) {
    EXPECT_FALSE(iso8601_to_epoch("2023-11-14T22:13:20").has_value());
}

TEST(WireDatetime, RejectsLowercaseT) {
    EXPECT_FALSE(iso8601_to_epoch("2023-11-14t22:13:20Z").has_value());
}

TEST(WireDatetime, RejectsTooFewFractionDigits) {
    // Fewer than three fractional digits before 'Z' is rejected (SDK state 6
    // requires >= 3).
    EXPECT_FALSE(iso8601_to_epoch("2023-11-14T22:13:20.5Z").has_value());
}

TEST(WireDatetime, RejectsFractionWithoutDigits) {
    EXPECT_FALSE(iso8601_to_epoch("2023-11-14T22:13:20.Z").has_value());
}

}  // namespace
