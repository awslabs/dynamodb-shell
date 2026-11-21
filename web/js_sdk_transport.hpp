// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// js_sdk_transport.hpp — the browser edition's wire Transport (P9, D-P9-02-2).
//
// JsSdkTransport hands each serialized AWS-JSON-1.0 request to the page's
// AWS SDK for JavaScript v3 bridge (web/sdk-bridge.mjs) through three JS hooks
// on the Emscripten Module:
//
//   send(op, body)          -> Module.ddbshSend(op, body): Promise<string>
//                              (Asyncify suspends the C++ stack across the await)
//   on_connect(region, url) -> Module.ddbshConnect(region, url): string, "" = ok
//   descriptor()            -> Module.ddbshDescriptor(): string
//
// The SDK signs, retries and sends; the engine's ddbsh-wire codec still
// serializes every request and parses every response, so the raw body that
// comes back (success or a `__type` error document) is exactly what the codec
// expects. Credentials never cross this seam.
//
// Web-private (lives in web/, not include/ddbsh/): the only translation unit
// that defines these members, js_sdk_transport.cpp, is compiled solely for the
// Emscripten target, and nothing outside the WASM build names the type.

#ifndef DDBSH_WEB_JS_SDK_TRANSPORT_HPP
#define DDBSH_WEB_JS_SDK_TRANSPORT_HPP

#include <string>
#include <string_view>

#include "ddbsh/result.hpp"
#include "ddbsh/wire/transport.hpp"

namespace ddbsh::web {

class JsSdkTransport : public ddbsh::wire::Transport {
public:
    /// Send one request through Module.ddbshSend. A resolved Promise yields the
    /// raw response body (the codec maps a `__type` document to an Error). A
    /// rejected Promise (no HTTP response: network/CORS failure, no or expired
    /// credentials, a blocked CONNECT) or a missing hook is a transport failure:
    /// Errc::service_error, "wire transport: <message>".
    Result<std::string> send(std::string_view operation, std::string_view body) override;

    /// Re-target the JS SDK client (CONNECT). A non-empty reason from
    /// Module.ddbshConnect is returned as Errc::service_error with that reason;
    /// the bridge has already failed closed (D-P9-02-6). Session ignores the
    /// result (CONNECT is best-effort, F12); web/bindings.cpp's WebWireClient
    /// surfaces it to the user as a `browser:` line.
    Result<void> on_connect(const std::string& region, const std::string& endpoint) override;

    /// SHOW CONNECTION backend line from Module.ddbshDescriptor (§5.4), e.g.
    /// "wire/aws-sdk-js 3.1174.0 -> https://dynamodb.us-east-1.amazonaws.com
    /// (default); credentials ASIA…WXYZ expires 2026-11-20T23:03:00.000Z".
    std::string descriptor() const override;
};

}  // namespace ddbsh::web

#endif  // DDBSH_WEB_JS_SDK_TRANSPORT_HPP
