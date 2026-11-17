// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transport_browser.cpp — the Asyncify bridge for SubmitTransport (P6d).
//
// Compiled ONLY for the Emscripten web target (the whole file is guarded by
// `#if defined(__EMSCRIPTEN__)`), so a host build that happens to add it to a
// source list simply yields an empty TU. It defines ddbsh::wire::SubmitTransport
// ::send by bridging to the JS global `Module.submit(operation, body)` Promise
// via one `EM_ASYNC_JS` function. Asyncify (`-sASYNCIFY`) suspends the whole C++
// stack across each `await`, so the engine's synchronous IDDBClient call path
// drives the async in-browser backend transparently (D-P6-12).

#if defined(__EMSCRIPTEN__)

#include <cstdlib>
#include <string>
#include <string_view>

#include <emscripten.h>

#include "ddbsh/wire/submit_transport.hpp"

// Asyncify-suspending call into the JS Promise `Module.submit(op, body)`.
// Returns a malloc'd, NUL-terminated C string the C++ side owns and frees, or
// nullptr on a transport-level failure (Module.submit missing, threw, or
// rejected). A service-level error is NOT a failure here: ExtendDB resolves it
// as a `__type` document in the body, which the codec maps downstream (the
// transport returns the body verbatim either way).
EM_ASYNC_JS(char*, ddbsh_wire_submit, (const char* op, const char* body), {
    try {
        if (typeof Module === "undefined" || typeof Module.submit !== "function") {
            return 0;  // bridge not wired
        }
        const o = UTF8ToString(op);
        const b = UTF8ToString(body);
        const resp = await Module.submit(o, b);
        if (typeof resp !== "string") {
            return 0;  // adapter contract is Promise<string>
        }
        const len = lengthBytesUTF8(resp) + 1;
        const buf = _malloc(len);
        stringToUTF8(resp, buf, len);
        return buf;
    } catch (e) {
        return 0;  // transport-level failure (backend threw / rejected)
    }
});

namespace ddbsh::wire {

Result<std::string> SubmitTransport::send(std::string_view operation, std::string_view body) {
    // string_view args are not guaranteed NUL-terminated; materialize before
    // crossing to JS.
    const std::string op(operation);
    const std::string b(body);
    char* raw = ddbsh_wire_submit(op.c_str(), b.c_str());
    if (raw == nullptr) {
        // Transport-level failure. Use the same Errc the host-side transport
        // doubles use for an unreachable backend (service_error), so the
        // rendered message is consistent across backends (D-P6d-1).
        return fail(Errc::service_error, "wire transport: backend submit failed");
    }
    std::string resp(raw);
    std::free(raw);
    // Success body OR a `__type` error document — the codec (downstream in
    // WireClient's parse_*) decides which. request_id stays nullopt on the wire
    // path (D-P6b-2).
    return resp;
}

std::string SubmitTransport::descriptor() const { return "wire/submit -> in-tab ExtendDB"; }

}  // namespace ddbsh::wire

#endif  // defined(__EMSCRIPTEN__)
