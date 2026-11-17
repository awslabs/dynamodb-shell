// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// datetime.cpp — SDK-free epoch<->ISO-8601 datetime codec (see datetime.hpp).
//
// The civil<->days conversions are Howard Hinnant's well-known algorithms
// (http://howardhinnant.github.io/date_algorithms.html), which map a
// proleptic-Gregorian (year, month, day) to a day count relative to
// 1970-01-01 and back, exactly as the SDK's underlying timegm does for the
// ranges DynamoDB produces. The ISO-8601 grammar accepted here mirrors the
// AWS-C-common parser (s_parse_iso_8601 in aws-c-common/source/date_time.c):
// lenient separators, optional time, fractional seconds discarded, trailing Z
// or +/-HH[:]MM offset.

#include "wire/datetime.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ddbsh::wire {
namespace {

// Days from 1970-01-01 to the given civil date (y, m in [1,12], d in [1,31]).
// Howard Hinnant's days_from_civil; valid across the full proleptic Gregorian
// range. `y` is the full year (e.g. 2026).
long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// Civil date (year, month [1,12], day [1,31]) from a day count relative to
// 1970-01-01. Howard Hinnant's civil_from_days (the inverse of above).
void civil_from_days(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);                // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                       // [0, 11]
    d = doy - (153 * mp + 2) / 5 + 1;                              // [1, 31]
    m = mp + (mp < 10 ? 3 : -9);                                   // [1, 12]
    y += (m <= 2);
}

// A tiny forward cursor over the ISO string, mirroring the AWS-C-common
// parser's primitives (read N digits, optional separator, read one char).
struct Cursor {
    const char* p;
    const char* end;

    bool read_digits(int n, int& out) {
        if (end - p < n) {
            return false;
        }
        int v = 0;
        for (int i = 0; i < n; ++i) {
            const char c = p[i];
            if (c < '0' || c > '9') {
                return false;
            }
            v = v * 10 + (c - '0');
        }
        p += n;
        out = v;
        return true;
    }

    bool eof() const { return p >= end; }

    bool read_char(char& c) {
        if (eof()) {
            return false;
        }
        c = *p++;
        return true;
    }
};

}  // namespace

std::optional<double> iso8601_to_epoch(std::string_view iso) {
    // Mirror the AWS SDK's Aws::Utils::DateTime ISO_8601DateParser
    // (aws-cpp-sdk-core DateTimeCommon.cpp), the oracle the adapter's
    // RestoreTableToPointInTime path uses. That parser is STRICT:
    //   YYYY '-' MM '-' DD 'T' HH ':' MM ':' SS  then one of
    //     'Z'                         (UTC, no fraction), or
    //     '.' <3..9 digits> 'Z'       (UTC, first 3 digits are milliseconds).
    // A non-UTC offset is treated by the SDK as local time (machine-TZ
    // dependent) and cannot be produced by ddbsh's T_TIMESTAMP lexer
    // ("YYYY-MM-DDThh:mm:ssZ" only), so it is rejected here. SecondsWithMSPrecision
    // returns whole-second + milliseconds/1000.
    Cursor cur{iso.data(), iso.data() + iso.size()};

    int year = 0, month = 0, mday = 0, hour = 0, minute = 0, second = 0;
    char sep = 0;
    if (!cur.read_digits(4, year) || !cur.read_char(sep) || sep != '-') {
        return std::nullopt;
    }
    if (!cur.read_digits(2, month) || !cur.read_char(sep) || sep != '-') {
        return std::nullopt;
    }
    if (!cur.read_digits(2, mday) || !cur.read_char(sep) || sep != 'T') {
        return std::nullopt;
    }
    if (!cur.read_digits(2, hour) || !cur.read_char(sep) || sep != ':') {
        return std::nullopt;
    }
    if (!cur.read_digits(2, minute) || !cur.read_char(sep) || sep != ':') {
        return std::nullopt;
    }
    if (!cur.read_digits(2, second)) {
        return std::nullopt;
    }

    int milliseconds = 0;
    char c = 0;
    if (!cur.read_char(c)) {
        return std::nullopt;  // no zone designator
    }
    if (c == '.') {
        // Fractional seconds: at least 3 digits (the SDK requires >= 3 before
        // the zone), first 3 taken as milliseconds, up to 9 total, then 'Z'.
        int frac_digits = 0;
        int ms = 0;
        while (!cur.eof() && *cur.p >= '0' && *cur.p <= '9') {
            if (frac_digits < 3) {
                ms = ms * 10 + (*cur.p - '0');
            }
            ++cur.p;
            ++frac_digits;
        }
        if (frac_digits < 3 || frac_digits > 9) {
            return std::nullopt;
        }
        milliseconds = ms;
        if (!cur.read_char(c)) {
            return std::nullopt;
        }
    }
    if (c != 'Z' || !cur.eof()) {
        return std::nullopt;  // only the UTC 'Z' form is accepted (and lexed)
    }

    const long long days =
        days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(mday));
    const long long epoch = days * 86400 + hour * 3600 + minute * 60 + second;
    return static_cast<double>(epoch) + static_cast<double>(milliseconds) / 1000.0;
}

std::string epoch_to_iso8601(double epoch_seconds) {
    // Truncate toward negative infinity to whole seconds (the ISO_8601 format
    // the SDK uses is second-precision).
    const long long total = static_cast<long long>(std::floor(epoch_seconds));
    long long days = total / 86400;
    long long rem = total % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    long long year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, year, month, day);
    const int hour = static_cast<int>(rem / 3600);
    const int minute = static_cast<int>((rem % 3600) / 60);
    const int second = static_cast<int>(rem % 60);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d:%02dZ", year, month, day, hour,
                  minute, second);
    return std::string(buf);
}

}  // namespace ddbsh::wire
