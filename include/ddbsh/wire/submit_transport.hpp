// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// submit_transport.hpp — the browser (Emscripten) wire backend (P6d, D-P6d-3).
//
// A ddbsh::wire::Transport that bridges the engine's SYNCHRONOUS IDDBClient
// call path to the JS global `Module.submit(operation, body) -> Promise<string>`
// via Emscripten Asyncify (D-P6-12). One `IDDBClient` call that fans out to many
// backend calls (Query/Scan paging, bulk UPDATE/DELETE) suspends per backend
// call transparently — Asyncify unwinds and rewinds the whole C++ stack on each
// `await`.
//
// Browser-only by construction: this header declares the portable, SDK-free,
// Emscripten-free SURFACE so web/bindings.cpp (and a host unit test that wants
// to name the type) can see it, but the only translation unit that DEFINES
// `send()` is web/transport_browser.cpp, compiled solely for the WASM target and
// guarded by `#if defined(__EMSCRIPTEN__)`. Linking this on any non-WASM target
// is a link error by design — the single caller is the Emscripten web target.
// Keeping the definition out of ddbsh-wire preserves the portable library's
// invariant that nothing under wire/ pulls <emscripten.h> (the one sanctioned
// exception is WireClient::poll_delay's pre-ratified `#if`-gated arm, D-P6c-6).

#ifndef DDBSH_WIRE_SUBMIT_TRANSPORT_HPP
#define DDBSH_WIRE_SUBMIT_TRANSPORT_HPP

#include <string>
#include <string_view>

#include "ddbsh/result.hpp"
#include "ddbsh/wire/transport.hpp"

namespace ddbsh::wire {

/// A Transport that bridges to the JS global `Module.submit(op, body)` Promise
/// via Emscripten Asyncify. A service-level error arrives as a success-resolved
/// `__type` document in the body (the codec maps it); a thrown/rejected
/// `Module.submit` (or an unwired bridge) is a transport-level failure that
/// `send()` returns as an Error (D-P6d-1).
class SubmitTransport : public Transport {
public:
    Result<std::string> send(std::string_view operation, std::string_view body) override;

    /// SHOW CONNECTION descriptor (phase 6f, D-P6f-9): the in-tab ExtendDB
    /// binding driven through `Module.submit`.
    std::string descriptor() const override;
};

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_SUBMIT_TRANSPORT_HPP
