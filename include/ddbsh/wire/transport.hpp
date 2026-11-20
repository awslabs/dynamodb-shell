// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transport.hpp — the ddbsh-wire backend seam (P6b; D-P6-A7/A8, D-P6b-3).
//
// A single, minimal abstraction over "send an AWS-JSON-1.0 request, get a
// response body back". WireClient (wire_client.hpp) drives serialize -> send ->
// parse through it. It is deliberately `(operation, body) -> body`, the exact
// shape every wire backend honors. Today's implementation is the browser
// edition's JsSdkTransport (web/js_sdk_transport.hpp, P9): the page's AWS SDK
// for JavaScript signs and sends the engine's body to Amazon DynamoDB and
// returns the raw response body; the test doubles (testing/canned_transport.hpp)
// are the others.
//
// Returning only the body (not headers/status) keeps the seam minimal and
// matches DynamoDB's AWS-JSON-1.0 error shape, a `__type` document in the
// response body — the codec inspects the body to decide success vs error. The
// one price is that Error.request_id is left nullopt on the wire path (D-P6b-2),
// which no golden path observes. A richer return type (e.g.
// `struct WireResponse { std::string body; std::optional<std::string>
// request_id; }`) behind the same method is a later, additive change if the
// browser path ever wants request-id fidelity (deferred, D-P9-02-10).
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
/// it to an Error). Synchronous from the caller's view — in the browser the
/// async SDK call is bridged by Asyncify inside the WASM transport.
class Transport {
public:
    virtual ~Transport() = default;

    /// Send `body` for `operation` (the X-Amz-Target suffix, e.g. "GetItem",
    /// "Query") and return the response body.
    virtual Result<std::string> send(std::string_view operation, std::string_view body) = 0;

    /// Notify the transport that a CONNECT changed the target region/endpoint
    /// (phase 6f, D-P6f-4). WireClient::connect calls this after it records the
    /// new state, so a backend-bound transport can re-target before the next
    /// send(). The browser transport rebuilds its JS SDK client for the new
    /// region/endpoint (and refuses non-DynamoDB endpoints, P9 D-P9-02-6); the
    /// test doubles need no reconnection, so the default is a no-op success. A
    /// returned Error is non-fatal at the Session layer (CONNECT is
    /// best-effort, mirroring the original's reconnect()).
    virtual Result<void> on_connect(const std::string& region, const std::string& endpoint) {
        (void)region;
        (void)endpoint;
        return {};
    }

    /// A short, SDK-free description of this transport for SHOW CONNECTION
    /// (phase 6f, D-P6f-9): the transport kind and, where known, its backend
    /// binding (e.g. "wire/aws-sdk-js 3.1174.0 -> https://dynamodb.us-east-1.
    /// amazonaws.com (default); credentials ASIA…WXYZ"). Default returns a
    /// generic label a concrete transport refines. Purely informational;
    /// contacts nothing.
    virtual std::string descriptor() const { return "wire transport"; }
};

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_TRANSPORT_HPP
