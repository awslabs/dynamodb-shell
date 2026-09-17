// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// version.cpp — assembles the project version string.
//
// Mirrors the original's convention (arch §10): a trailing zero segment is
// dropped, so 0.9.1.0 renders "0.9.1" and 0.9.0 renders "0.9". Uses std::format
// (no fixed stack buffers / sprintf — coding-standards).

#include "ddbsh/version.hpp"

#include <format>

#include "version_config.hpp"

namespace ddbsh {

std::string version() {
    if constexpr (DDBSH_VERSION_TWEAK > 0) {
        return std::format("{}.{}.{}.{}", DDBSH_VERSION_MAJOR, DDBSH_VERSION_MINOR,
                           DDBSH_VERSION_PATCH, DDBSH_VERSION_TWEAK);
    } else if constexpr (DDBSH_VERSION_PATCH > 0) {
        return std::format("{}.{}.{}", DDBSH_VERSION_MAJOR, DDBSH_VERSION_MINOR,
                           DDBSH_VERSION_PATCH);
    } else {
        return std::format("{}.{}", DDBSH_VERSION_MAJOR, DDBSH_VERSION_MINOR);
    }
}

}  // namespace ddbsh
