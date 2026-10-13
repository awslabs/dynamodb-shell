// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// prefix.hpp — the deterministic per-statement alias prefix generator.
//
// PRIVATE engine header (not installed). Reproduces the original
// CDDBSh::st_unique_prefix() byte-for-byte (ddbsh.cpp): a 3-character,
// little-endian base-52 counter over [a-zA-Z]. The sequence is required for
// byte-identical EXPLAIN output (arch §14 item 1): counter 0 -> "aaa",
// 1 -> "baa", 2 -> "caa", ... .
//
// The original held the counter in the CDDBSh singleton (a process global,
// T1). Here the counter is owned by the Session (per the Phase-2b rules: "the
// per-statement prefix source lives on Session, not a global"); this header
// only maps a counter value to its prefix string, so it is pure and
// unit-testable.

#ifndef DDBSH_SRC_EXPRESSIONS_PREFIX_HPP
#define DDBSH_SRC_EXPRESSIONS_PREFIX_HPP

#include <string>

namespace ddbsh::engine {

/// Base-52 alphabet, matching the original DDBSH_UNIQUE_PREFIX_CHARS order
/// (lowercase a-z then uppercase A-Z).
inline constexpr char kPrefixChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

/// Number of characters in a generated prefix (the original
/// DDBSH_UNIQUE_PREFIX_LEN).
inline constexpr int kPrefixLength = 3;

/// Map a 0-based counter to its 3-char base-52 prefix, little-endian — exactly
/// as the original st_unique_prefix() computed it. @param counter the
/// per-session statement counter value (0 for the first statement).
inline std::string unique_prefix(unsigned long counter) {
    constexpr unsigned long base = sizeof(kPrefixChars) - 1;  // 52 (drop NUL)
    std::string prefix;
    unsigned long value = counter;
    for (int ix = 0; ix < kPrefixLength; ++ix) {
        prefix += kPrefixChars[value % base];
        value /= base;
    }
    return prefix;
}

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXPRESSIONS_PREFIX_HPP
