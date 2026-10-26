// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// version.hpp — the reimplementation's project version (D15).
//
// The reimplementation carries a single project version starting at 0.9.1
// (human-ratified, amended from the proposed 0.9.0). The numeric components
// are configured from CMake (project VERSION) via version_config.hpp; the
// string form drops a trailing .0 segment to match the original's convention
// (arch §10: 0.8.1.0 -> "0.8.1").

#ifndef DDBSH_VERSION_HPP
#define DDBSH_VERSION_HPP

#include <string>

namespace ddbsh {

/// The project version string (e.g. "0.9.1"), trailing ".0" segments trimmed.
std::string version();

}  // namespace ddbsh

#endif  // DDBSH_VERSION_HPP
