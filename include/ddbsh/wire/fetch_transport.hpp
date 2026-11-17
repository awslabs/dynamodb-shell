// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// fetch_transport.hpp — the browser (Emscripten) wire backend for the
// real-backend edition (P6f, D-P6f-1/D-P6f-2).
//
// A ddbsh::wire::Transport that bridges the engine's SYNCHRONOUS IDDBClient
// call path to the JS global `Module.fetchDdb(operation, body) -> Promise<
// string>` via Emscripten Asyncify (D-P6-12), exactly as SubmitTransport
// bridges to `Module.submit`. Where SubmitTransport talks to an in-tab ExtendDB
// blob, FetchTransport talks to `ddbsh-web-proxy`: the JS `fetchDdb` hook POSTs
// the AWS-JSON-1.0 body to the proxy's `/ddb` (carrying the X-Amz-Target
// operation + the per-session header), the proxy SigV4-signs and forwards to
// the session's real DynamoDB / ExtendDB endpoint, and the response body comes
// back for the codec to parse. Credentials never reach the browser — the only
// thing crossing this seam is the operation name and the wire body.
//
// This split — one engine/codec/UI, two leaf transports — is the whole A′
// architecture (D-P6f-2): the `ddbsh-wasm` edition links FetchTransport, the
// `ddbsh-wasm-extenddb-wasm` edition links SubmitTransport, and nothing else
// differs between them.
//
// Browser-only by construction, like submit_transport.hpp: this header declares
// the portable, SDK-free, Emscripten-free SURFACE so web/bindings.cpp (and a
// host unit test that wants to name the type) can see it, but the only
// translation unit that DEFINES `send()` is web/transport_fetch.cpp, compiled
// solely for the WASM target and guarded by `#if defined(__EMSCRIPTEN__)`.
// Linking this on any non-WASM target is a link error by design. Keeping the
// definition out of ddbsh-wire preserves the portable library's invariant that
// nothing under wire/ pulls <emscripten.h>.

#ifndef DDBSH_WIRE_FETCH_TRANSPORT_HPP
#define DDBSH_WIRE_FETCH_TRANSPORT_HPP

#include <string>
#include <string_view>

#include "ddbsh/result.hpp"
#include "ddbsh/wire/transport.hpp"

namespace ddbsh::wire {

/// A Transport that bridges to the JS global `Module.fetchDdb(op, body)` Promise
/// via Emscripten Asyncify. The JS hook performs the actual `fetch()` to the
/// local `ddbsh-web-proxy`, which signs and forwards to the real backend. A
/// service-level error arrives as a success-resolved `__type` document in the
/// body (the codec maps it); a thrown/rejected `Module.fetchDdb` (an unreachable
/// proxy, an unwired bridge, a non-2xx HTTP status the hook rejects on) is a
/// transport-level failure that `send()` returns as an Error (mirrors
/// SubmitTransport / D-P6d-1).
class FetchTransport : public Transport {
public:
    Result<std::string> send(std::string_view operation, std::string_view body) override;

    /// Forward a CONNECT to the proxy as a control request (phase 6f, D-P6f-4).
    /// Bridges to the JS global `Module.connectDdb(region, endpoint) ->
    /// Promise<string>` via Asyncify: the hook POSTs to the proxy's `/connect`
    /// so later `/ddb` sends sign for the new target. The resolved string is
    /// empty on success and an error message on failure; a missing hook is
    /// treated as a no-op success (the glue may manage the connection out of
    /// band). A thrown/rejected hook is a non-fatal transport error.
    Result<void> on_connect(const std::string& region, const std::string& endpoint) override;

    /// SHOW CONNECTION descriptor (phase 6f, D-P6f-9): the proxy-forwarded
    /// backend binding. P6f-a reports the static transport kind; a later slice
    /// may enrich it with the live proxy version/endpoint once the proxy
    /// exposes them (TODO(phase-6f)).
    std::string descriptor() const override;
};

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_FETCH_TRANSPORT_HPP
