// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// bindings.cpp — the embind surface for the browser edition (P6d; P9).
//
// Exposes a single `DdbshSession` class to JavaScript. It owns the whole
// SDK-free engine stack — a JsSdkTransport (the AWS SDK for JavaScript bridge,
// web/js_sdk_transport.hpp), a WireClient over it, and a ddbsh::Session — and
// runs input through the SAME terminal-free driver the CLI uses
// (cli/repl.hpp), buffering the rendered bytes into a RunResult instead of
// writing to file descriptors. No engine logic lives in JS (tenet 3): JS owns
// only module instantiation, the credentials, and the SDK bridge hooks.
//
// Two entry points (D-P9-02-11):
//   run(text)                       the UI path: one submission through
//                                   run_batch (no terminator lines, no
//                                   stop-on-quit across submissions).
//   runScript(text, abortOnError)   the CLI `-c` / `-f` path: run_source over
//                                   the text, exactly as the CLI drives it
//                                   (terminator lines, stop-on-quit, -e). The
//                                   golden gate (web/ddbsh-node.mjs) uses it.
// Both return RunResult.segments: the rendered stdout/stderr text in emission
// order, so a consumer that merges the streams (the golden runner's 2>&1, the
// UI transcript) sees the same interleaving as the CLI.
//
// WASM-only: this TU is compiled solely for the Emscripten web target.

#if defined(__EMSCRIPTEN__)

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "ddbsh/config.hpp"
#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/wire/wire_client.hpp"
#include "js_sdk_transport.hpp"
#include "repl.hpp"

namespace {

// The JS-visible result of one run / runScript. `segments` is a JS array of
// {stream: "stdout"|"stderr", text} in emission order (adjacent same-stream
// text coalesced); `stdout`/`stderr` are the per-stream concatenations;
// `ok`/`status` follow the CLI's last-executed-statement exit semantics.
struct RunResult {
    std::string stdout_text;
    std::string stderr_text;
    emscripten::val segments = emscripten::val::array();
    bool ok = true;
    int status = 0;

    void append(const char* stream, const std::string& text) {
        if (text.empty()) {
            return;
        }
        (std::string_view(stream) == "stdout" ? stdout_text : stderr_text) += text;
        const int n = segments["length"].as<int>();
        if (n > 0) {
            emscripten::val last = segments[n - 1];
            if (last["stream"].as<std::string>() == stream) {
                last.set("text", last["text"].as<std::string>() + text);
                return;
            }
        }
        emscripten::val seg = emscripten::val::object();
        seg.set("stream", std::string(stream));
        seg.set("text", text);
        segments.call<void>("push", seg);
    }
};

// The browser edition's WireClient. Two differences from the base class:
//   * retries belong to the JS SDK (D-P9-02-3): the base WireClient retry
//     budget is cut to a single attempt, so sustained throttling costs the
//     SDK's 11 attempts, not 11 x 11;
//   * a CONNECT the bridge rejected (D-P9-02-6) is remembered so the sink can
//     tell the user why (the engine itself ignores connect() failures, F12).
class WebWireClient : public ddbsh::wire::WireClient {
public:
    explicit WebWireClient(ddbsh::wire::Transport& t) : WireClient(t) { max_retry_attempts_ = 1; }

    ddbsh::Result<void> connect(const std::string& region, const std::string& endpoint) override {
        auto r = WireClient::connect(region, endpoint);
        if (!r.has_value()) {
            pending_notice_ = "browser: CONNECT rejected: " + r.error().message + "\n";
        }
        return r;
    }

    // The pending CONNECT notice, once (empty when there is none).
    std::string take_notice() { return std::exchange(pending_notice_, std::string()); }

private:
    std::string pending_notice_;
};

// Buffers each emitted fragment into a RunResult — the string counterpart to
// the CLI's FD-writing sink (same render / render_stderr split). A rejected
// CONNECT's notice follows that statement's CONNECT token.
struct BufferingSink : ddbsh::OutcomeSink {
    const ddbsh::render::Renderer& renderer;
    WebWireClient& client;
    RunResult& result;
    BufferingSink(const ddbsh::render::Renderer& r, WebWireClient& c, RunResult& res)
        : renderer(r), client(c), result(res) {}
    void emit(const ddbsh::ExecOutcome& fragment) override {
        result.append("stdout", renderer.render(fragment));
        result.append("stderr", renderer.render_stderr(fragment));
        result.append("stderr", client.take_notice());
    }
};

}  // namespace

// A browser-side DynamoDB shell session. Construct one per tab; call run(text)
// per submission.
class DdbshSession {
public:
    explicit DdbshSession(std::string region)
        : client_(transport_),
          session_(ddbsh::SessionConfig{std::move(region), /*endpoint=*/""}, client_) {}

    // The UI path: one submission of ';'-separated statements. abort_on_error
    // is fixed false: the browser runs one submission at a time, so there is no
    // multi-batch abort policy to honor.
    RunResult run(const std::string& text) {
        RunResult result;
        BufferingSink sink(renderer_, client_, result);
        ddbsh::cli::BatchStatus st = ddbsh::cli::run_batch(
            session_, renderer_, text, /*abort_on_error=*/false, sink,
            [&](const std::string& rendered) { result.append("stderr", rendered); });
        const bool failed = st.last_error.value_or(false);
        result.ok = !failed;
        result.status = failed ? 1 : 0;
        return result;
    }

    // The CLI `-c` / `-f` path (run_source): status is the CLI's exit code.
    RunResult run_script(const std::string& text, bool abort_on_error) {
        RunResult result;
        BufferingSink sink(renderer_, client_, result);
        std::istringstream in(text);
        ddbsh::cli::StreamLineSource src(in);
        ddbsh::cli::Terminator term;  // the CLI default (`go`); the shim passes no -t
        result.status = ddbsh::cli::run_source(
            session_, renderer_, src, term, abort_on_error, sink,
            [&](const std::string& rendered) { result.append("stderr", rendered); });
        result.ok = result.status == 0;
        return result;
    }

    // Prompt hints for the UI; CONNECT updates these live (B10 / D-2j-2a).
    std::string region() const { return session_.connection_hint().region; }
    bool endpoint_overridden() const { return session_.connection_hint().endpoint_overridden; }

private:
    // Declaration order is a correctness requirement, not style: WireClient
    // borrows transport_, Session borrows client_. Declaring transport_ ->
    // client_ -> session_ makes construction (and reverse-order destruction)
    // respect the borrow chain (the "must outlive" contracts in
    // wire_client.hpp / session.hpp).
    ddbsh::web::JsSdkTransport transport_;
    WebWireClient client_;
    ddbsh::Session session_;
    ddbsh::render::Renderer renderer_;
};

EMSCRIPTEN_BINDINGS(ddbsh) {
    emscripten::value_object<RunResult>("RunResult")
        .field("stdout", &RunResult::stdout_text)
        .field("stderr", &RunResult::stderr_text)
        .field("segments", &RunResult::segments)
        .field("ok", &RunResult::ok)
        .field("status", &RunResult::status);
    emscripten::class_<DdbshSession>("DdbshSession")
        .constructor<std::string>()
        .function("run", &DdbshSession::run)
        .function("runScript", &DdbshSession::run_script)
        .function("region", &DdbshSession::region)
        .function("endpointOverridden", &DdbshSession::endpoint_overridden);
}

// A trivial entry point. The embind surface is registered by static
// initializers (EMSCRIPTEN_BINDINGS) before main runs, so the module is fully
// usable from JS once the factory promise resolves. The runtime is kept alive
// after main returns (EXIT_RUNTIME defaults off).
int main() {
    return 0;
}

#endif  // defined(__EMSCRIPTEN__)
