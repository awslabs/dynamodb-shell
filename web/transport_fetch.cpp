// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transport_fetch.cpp — the Asyncify bridge for FetchTransport (P6f, D-P6f-1).
//
// Compiled ONLY for the Emscripten web target (the whole file is guarded by
// `#if defined(__EMSCRIPTEN__)`), so a host build that happens to add it to a
// source list simply yields an empty TU. It defines ddbsh::wire::FetchTransport
// ::send by bridging to the JS global `Module.fetchDdb(operation, body)`
// Promise via one `EM_ASYNC_JS` function — the exact shape of the SubmitTransport
// bridge, pointed at the proxy fetch instead of the in-tab ExtendDB submit.
// Asyncify (`-sASYNCIFY`) suspends the whole C++ stack across each `await`, so
// the engine's synchronous IDDBClient call path drives the async fetch
// transparently (D-P6-12). Credentials never cross this seam; the JS hook and
// the native proxy own signing.

#if defined(__EMSCRIPTEN__)

#include <cstdlib>
#include <string>
#include <string_view>

#include <emscripten.h>

#include "ddbsh/wire/fetch_transport.hpp"

// Asyncify-suspending call into the JS Promise `Module.fetchDdb(op, body)`.
// Returns a malloc'd, NUL-terminated C string the C++ side owns and frees, or
// nullptr on a transport-level failure (Module.fetchDdb missing, threw, or
// rejected — e.g. the proxy is unreachable or returned a non-2xx status the
// hook rejects on). A service-level error is NOT a failure here: the proxy
// relays DynamoDB/ExtendDB's `__type` document verbatim in the body, which the
// codec maps downstream (the transport returns the body either way).
EM_ASYNC_JS(char*, ddbsh_wire_fetch, (const char* op, const char* body), {
    try {
        if (typeof Module === "undefined" || typeof Module.fetchDdb !== "function") {
            return 0;  // bridge not wired
        }
        const o = UTF8ToString(op);
        const b = UTF8ToString(body);
        const resp = await Module.fetchDdb(o, b);
        if (typeof resp !== "string") {
            return 0;  // hook contract is Promise<string>
        }
        const len = lengthBytesUTF8(resp) + 1;
        const buf = _malloc(len);
        stringToUTF8(resp, buf, len);
        return buf;
    } catch (e) {
        return 0;  // transport-level failure (proxy unreachable / rejected)
    }
});

// Asyncify-suspending call into the JS Promise `Module.connectDdb(region,
// endpoint)` (D-P6f-4). The hook POSTs to the proxy's `/connect` so later `/ddb`
// sends sign for the new target. Always returns a malloc'd, NUL-terminated C
// string the C++ side owns and frees: empty ("") on success, an error message
// otherwise. A missing hook is a no-op success (the glue may establish the
// proxy connection out of band at bootstrap); a thrown/rejected hook yields an
// error message. Credentials never cross this seam — only the target names.
EM_ASYNC_JS(char*, ddbsh_wire_connect, (const char* region, const char* endpoint), {
    function dup(s) {
        const str = (typeof s === "string") ? s : "";
        const len = lengthBytesUTF8(str) + 1;
        const buf = _malloc(len);
        stringToUTF8(str, buf, len);
        return buf;
    }
    try {
        if (typeof Module === "undefined" || typeof Module.connectDdb !== "function") {
            return dup("");  // no control hook wired -> no-op success
        }
        const r = UTF8ToString(region);
        const e = UTF8ToString(endpoint);
        const msg = await Module.connectDdb(r, e);
        return dup(typeof msg === "string" ? msg : "");
    } catch (ex) {
        return dup("proxy connect failed: " + ((ex && ex.message) ? ex.message : String(ex)));
    }
});

namespace ddbsh::wire {

Result<std::string> FetchTransport::send(std::string_view operation, std::string_view body) {
    // string_view args are not guaranteed NUL-terminated; materialize before
    // crossing to JS.
    const std::string op(operation);
    const std::string b(body);
    char* raw = ddbsh_wire_fetch(op.c_str(), b.c_str());
    if (raw == nullptr) {
        // Transport-level failure. Use the same Errc the other transports use
        // for an unreachable backend (service_error), so the rendered message
        // is consistent across backends (D-P6d-1 / D-P6f-1).
        return fail(Errc::service_error, "wire transport: proxy fetch failed");
    }
    std::string resp(raw);
    std::free(raw);
    // Success body OR a `__type` error document — the codec (downstream in
    // WireClient's parse_*) decides which. request_id stays nullopt on the wire
    // path (D-P6b-2).
    return resp;
}

std::string FetchTransport::descriptor() const { return "wire/fetch -> POST /ddb (proxy)"; }

Result<void> FetchTransport::on_connect(const std::string& region, const std::string& endpoint) {
    char* raw = ddbsh_wire_connect(region.c_str(), endpoint.c_str());
    if (raw == nullptr) {
        // Defensive: the bridge always returns a malloc'd string, but a null
        // (OOM) is treated as a transport-level failure.
        return fail(Errc::service_error, "wire transport: proxy connect failed");
    }
    std::string msg(raw);
    std::free(raw);
    if (!msg.empty()) {
        return fail(Errc::service_error, "wire transport: " + msg);
    }
    return {};
}

}  // namespace ddbsh::wire

#endif  // defined(__EMSCRIPTEN__)
