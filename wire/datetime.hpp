// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// datetime.hpp — a compact, SDK-free epoch<->ISO-8601 datetime codec internal
// to ddbsh-wire (P6c).
//
// WHY THIS EXISTS. The AWS adapter produces the DESCRIBE/SHOW report datetimes
// with Aws::Utils::DateTime::ToGmtString(ISO_8601) and parses the one request
// timestamp (RestoreTableToPointInTime.RestoreDateTime) with
// Aws::Utils::DateTime(str, ISO_8601). The SDK-free wire codec must reproduce
// both, field-for-field, without the SDK:
//
//   * iso8601_to_epoch — the inverse of what the RestoreDateTime serializer
//     needs: an ISO-8601 string -> epoch-seconds-with-ms double, matching
//     Aws::Utils::DateTime(str, ISO_8601).SecondsWithMSPrecision(). It mirrors
//     the SDK's STRICT Aws::Utils::DateTime ISO_8601DateParser
//     (aws-cpp-sdk-core DateTimeCommon.cpp), which accepts only
//     "YYYY-MM-DDTHH:MM:SS" followed by 'Z' (UTC) or ".<3..9 digits>Z" (UTC
//     with milliseconds). A non-UTC offset is treated by the SDK as local time
//     and cannot be produced by ddbsh's T_TIMESTAMP lexer ("YYYY-MM-DDThh:mm:ssZ"
//     only), so it is rejected. Returns nullopt on an unparseable string (the
//     SDK's WasParseSuccessful() == false path), which the WireClient surfaces
//     as the same invalid_argument Error the adapter returns.
//
//   * epoch_to_iso8601 — the inverse the response parsers need: a DynamoDB
//     response timestamp (epoch-seconds JSON number) -> the exact string
//     ToGmtString(ISO_8601) produces, "YYYY-MM-DDTHH:MM:SSZ" (UTC, second
//     precision, trailing Z). The engine's report structs store these as opaque
//     strings and the Renderer prints them verbatim, so the only fidelity
//     requirement is that epoch_to_iso8601(resp_epoch) equals what the adapter
//     produced from the same instant.
//
// PORTABLE. The civil<->days conversion is Howard Hinnant's algorithm (valid
// for the full proleptic Gregorian range), not timegm/gmtime_r, so there is no
// platform dependence and the Emscripten/WASM target (P6d) needs no change.
//
// SDK-FREE + TERMINAL-FREE. Lives under wire/, scanned by the engine boundary
// check: no terminal, no process globals, no I/O.

#ifndef DDBSH_WIRE_DATETIME_HPP
#define DDBSH_WIRE_DATETIME_HPP

#include <optional>
#include <string>
#include <string_view>

namespace ddbsh::wire {

/// Format a DynamoDB epoch-seconds value (a response JSON number) as the exact
/// string Aws::Utils::DateTime::ToGmtString(ISO_8601) produces:
/// "YYYY-MM-DDTHH:MM:SSZ" (UTC, second precision, trailing 'Z'). Any fractional
/// part of `epoch_seconds` is truncated toward negative infinity to whole
/// seconds, matching the SDK's second-precision ISO format.
std::string epoch_to_iso8601(double epoch_seconds);

/// Parse an ISO-8601 timestamp to epoch-seconds, matching
/// Aws::Utils::DateTime(str, ISO_8601).SecondsWithMSPrecision(). Accepts the
/// SDK's strict ISO_8601DateParser grammar: "YYYY-MM-DDTHH:MM:SS" followed by
/// 'Z' (UTC), or ".<3..9 digits>Z" (UTC, the first three digits are
/// milliseconds). Returns nullopt for anything else — including a date without
/// a time, lenient separators, or a non-UTC +/-HH:MM offset — matching the
/// SDK's WasParseSuccessful() == false path for the forms ddbsh can produce.
std::optional<double> iso8601_to_epoch(std::string_view iso);

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_DATETIME_HPP
