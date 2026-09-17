// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// config.hpp — session configuration (SDK-free).
//
// Replaces the original CDDBSh singleton's process-global region/endpoint
// (T1). Constructed by the embedder and handed to a Session; no globals.

#ifndef DDBSH_CONFIG_HPP
#define DDBSH_CONFIG_HPP

#include <string>

namespace ddbsh {

/// Per-session configuration. The AWS region/endpoint are carried here for the
/// prompt and the CLI-side client construction; the engine itself does not
/// contact AWS (that is the injected IDDBClient's job).
struct SessionConfig {
    std::string region;
    std::string endpoint;  ///< empty unless an endpoint override is in effect
};

}  // namespace ddbsh

#endif  // DDBSH_CONFIG_HPP
