// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// js_sdk_transport.cpp — the Asyncify bridge for JsSdkTransport (P9, §2.3).
//
// Compiled ONLY for the Emscripten web target (the whole file is guarded by
// `#if defined(__EMSCRIPTEN__)`). Each member crosses to a JS hook the page or
// the Node shim installs on Module (web/sdk-bridge.mjs installHooks):
//
//   ddbsh_js_send       EM_ASYNC_JS  Module.ddbshSend(op, body) -> Promise<string>
//   ddbsh_js_connect    EM_JS (sync) Module.ddbshConnect(region, endpoint) -> string
//   ddbsh_js_descriptor EM_JS (sync) Module.ddbshDescriptor() -> string
//
// Every hook returns one malloc'd, NUL-terminated C string the C++ side frees.
// For send it is an envelope: '0' + body on success, '1' + message on failure,
// so a single crossing carries either outcome.

#if defined(__EMSCRIPTEN__)

#include "js_sdk_transport.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

#include <emscripten.h>

// The hook bodies are JavaScript; clang-format would rewrite `===` / `!==`.
// clang-format off
EM_ASYNC_JS(char*, ddbsh_js_send, (const char* op, const char* body), {
    function dup(s) {
        const len = lengthBytesUTF8(s) + 1;
        const buf = _malloc(len);
        stringToUTF8(s, buf, len);
        return buf;
    }
    if (typeof Module === "undefined" || typeof Module.ddbshSend !== "function") {
        return dup("1browser SDK bridge not initialized");
    }
    try {
        const resp = await Module.ddbshSend(UTF8ToString(op), UTF8ToString(body));
        if (typeof resp !== "string") {
            return dup("1browser SDK bridge returned no response body");
        }
        return dup("0" + resp);
    } catch (e) {
        return dup("1" + ((e && e.message) ? e.message : String(e)));
    }
});

EM_JS(char*, ddbsh_js_connect, (const char* region, const char* endpoint), {
    let msg = "";
    if (typeof Module === "undefined" || typeof Module.ddbshConnect !== "function") {
        msg = "browser SDK bridge not initialized";
    } else {
        try {
            const r = Module.ddbshConnect(UTF8ToString(region), UTF8ToString(endpoint));
            msg = (typeof r === "string") ? r : "";
        } catch (e) {
            msg = (e && e.message) ? e.message : String(e);
        }
    }
    const len = lengthBytesUTF8(msg) + 1;
    const buf = _malloc(len);
    stringToUTF8(msg, buf, len);
    return buf;
});

EM_JS(char*, ddbsh_js_descriptor, (), {
    let d = "wire/aws-sdk-js (bridge not initialized)";
    if (typeof Module !== "undefined" && typeof Module.ddbshDescriptor === "function") {
        try {
            const r = Module.ddbshDescriptor();
            if (typeof r === "string") {
                d = r;
            }
        } catch (e) {
            // keep the fallback label; descriptor() is informational only.
        }
    }
    const len = lengthBytesUTF8(d) + 1;
    const buf = _malloc(len);
    stringToUTF8(d, buf, len);
    return buf;
});
// clang-format on

namespace {

// Take ownership of a hook's malloc'd string.
std::string take(char* raw) {
    if (raw == nullptr) {
        return {};
    }
    std::string s(raw);
    std::free(raw);
    return s;
}

}  // namespace

namespace ddbsh::web {

Result<std::string> JsSdkTransport::send(std::string_view operation, std::string_view body) {
    // string_view args are not guaranteed NUL-terminated; materialize first.
    const std::string op(operation);
    const std::string b(body);
    std::string env = take(ddbsh_js_send(op.c_str(), b.c_str()));
    if (env.empty()) {
        return fail(Errc::service_error, "wire transport: browser SDK bridge returned nothing");
    }
    if (env[0] == '0') {
        // Success body OR a `__type` error document; the codec decides.
        // TODO(fidelity): the request id is not returned (Error.request_id
        // stays nullopt), so composed service errors lack the `<RID>` segment
        // the CLI prints. CORS exposes x-amzn-RequestId; closing this needs a
        // Transport return-type change (WireResponse). Deferred, D-P9-02-10.
        return env.substr(1);
    }
    return fail(Errc::service_error, "wire transport: " + env.substr(1));
}

Result<void> JsSdkTransport::on_connect(const std::string& region, const std::string& endpoint) {
    std::string reason = take(ddbsh_js_connect(region.c_str(), endpoint.c_str()));
    if (!reason.empty()) {
        return fail(Errc::service_error, reason);
    }
    return {};
}

std::string JsSdkTransport::descriptor() const {
    return take(ddbsh_js_descriptor());
}

}  // namespace ddbsh::web

#endif  // defined(__EMSCRIPTEN__)
