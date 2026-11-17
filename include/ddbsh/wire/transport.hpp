// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transport.hpp — the ddbsh-wire backend seam (P6b; D-P6-A7/A8, D-P6b-3).
//
// A single, minimal abstraction over "send an AWS-JSON-1.0 request, get a
// response body back". WireClient (wire_client.hpp) drives serialize -> send ->
// parse through it. It is deliberately `(operation, body) -> body`, the exact
// shape ExtendDB's submit(operation, body) -> Promise<string> honors, and the
// single contract all three eventual backends share (D-P6-A7):
//   1. in-WASM ExtendDB            [P6d]
//   2. local ExtendDB over fetch   [P6d]
//   3. real DynamoDB via SigV4+fetch [v2, security-gated]
//
// Returning only the body (not headers/status) keeps the seam identical across
// all three and matches ExtendDB resolving errors AS `__type` documents rather
// than rejections — the codec inspects the body to decide success vs error. The
// one price is that Error.request_id is left nullopt on the wire path (D-P6b-2),
// which no golden path observes. A richer return type (e.g.
// `struct WireResponse { std::string body; std::optional<std::string>
// request_id; }`) behind the same method is a later, additive change if the v2
// path ever wants request-id fidelity.
//
// SDK-free + terminal-free: this public header requires only a C++23 toolchain
// and the engine's SDK-free Result/Error types.

#ifndef DDBSH_WIRE_TRANSPORT_HPP
#define DDBSH_WIRE_TRANSPORT_HPP

#include <string>
#include <string_view>

#include "ddbsh/result.hpp"

namespace ddbsh::wire {

/// The wire backend seam. An implementation POSTs an AWS-JSON-1.0 request and
/// returns the response body. A transport-level failure (no connection, the
/// backend unreachable) is returned as an Error; a service-level failure
/// arrives as a successful `__type` error document in the body (the codec maps
/// it to an Error). Synchronous from the caller's view — the browser's async
/// submit() is bridged by Asyncify inside the P6d WASM transport.
class Transport {
public:
    virtual ~Transport() = default;

    /// Send `body` for `operation` (the X-Amz-Target suffix, e.g. "GetItem",
    /// "Query") and return the response body.
    virtual Result<std::string> send(std::string_view operation, std::string_view body) = 0;

    /// A short, SDK-free description of this transport for SHOW CONNECTION
    /// (phase 6f, D-P6f-9): the transport kind and, where known, its backend
    /// binding (e.g. "wire/fetch -> POST /ddb (proxy)",
    /// "wire/submit -> in-tab ExtendDB"). Default returns a generic label a
    /// concrete transport refines. Purely informational; contacts nothing.
    virtual std::string descriptor() const { return "wire transport"; }
};

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_TRANSPORT_HPP
