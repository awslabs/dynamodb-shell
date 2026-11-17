// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// bindings.cpp — the embind surface for the browser (P6d, D-P6d-5).
//
// Exposes a single `DdbshSession` class to JavaScript. It owns the whole
// SDK-free engine stack — a SubmitTransport (Asyncify bridge to
// `Module.submit`), a WireClient over it, and a ddbsh::Session — and runs one
// submission of `;`-separated statements through the SAME batch pipeline the
// CLI uses (ddbsh::cli::run_batch), buffering the rendered bytes into a
// RunResult instead of writing to file descriptors. No engine logic lives in
// JS (tenet 3): JS owns only module instantiation and the `Module.submit`
// wiring to the in-browser ExtendDB adapter.
//
// WASM-only: this TU is compiled solely for the Emscripten web target.

#if defined(__EMSCRIPTEN__)

#include <string>
#include <utility>

#include <emscripten/bind.h>

#include "ddbsh/config.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/wire/submit_transport.hpp"
#include "ddbsh/wire/wire_client.hpp"
#include "repl.hpp"

namespace {

// The JS-visible result of one DdbshSession::run. `stdout_text` carries the
// faithful shell result bytes (the Renderer's output, O-2); `stderr_text`
// carries diagnostics and error strings; `ok`/`status` mirror the CLI's
// last-executed-statement exit semantics (a later success resets an earlier
// failure).
struct RunResult {
    std::string stdout_text;
    std::string stderr_text;
    bool ok = true;
    int status = 0;
};

// Buffers each emitted fragment into the two result strings instead of writing
// to stdout/stderr — the string counterpart to the CLI's FD-writing sink. The
// result-byte / diagnostic split matches the CLI exactly (render vs
// render_stderr), so a transaction's ABORT (stdout) and its diagnostics
// (stderr) land in the right buffers.
struct BufferingSink : ddbsh::OutcomeSink {
    const ddbsh::render::Renderer& renderer;
    std::string& out;
    std::string& err;
    BufferingSink(const ddbsh::render::Renderer& r, std::string& o, std::string& e)
        : renderer(r), out(o), err(e) {}
    void emit(const ddbsh::ExecOutcome& fragment) override {
        out += renderer.render(fragment);
        err += renderer.render_stderr(fragment);
    }
};

}  // namespace

// A browser-side DynamoDB shell session. Construct one per logical connection;
// call run(text) per submission.
class DdbshSession {
public:
    explicit DdbshSession(std::string region)
        : client_(transport_),
          session_(ddbsh::SessionConfig{std::move(region), /*endpoint=*/""}, client_) {}

    // Run a submission of ';'-separated statements and return the rendered
    // result. abort_on_error is fixed false: the browser runs one submission at
    // a time, so there is no multi-batch abort policy to honor (the CLI's -e is
    // a whole-session concept).
    RunResult run(const std::string& text) {
        RunResult result;
        BufferingSink sink(renderer_, result.stdout_text, result.stderr_text);
        ddbsh::cli::BatchStatus st = ddbsh::cli::run_batch(
            session_, renderer_, text, /*abort_on_error=*/false, sink,
            [&](const std::string& rendered) { result.stderr_text += rendered; });
        const bool failed = st.last_error.value_or(false);
        result.ok = !failed;
        result.status = failed ? 1 : 0;
        return result;
    }

    // Prompt hints for a P6e UI; CONNECT updates these live (B10 / D-2j-2a).
    std::string region() const { return session_.connection_hint().region; }
    bool endpoint_overridden() const { return session_.connection_hint().endpoint_overridden; }

private:
    // Declaration order is a correctness requirement, not style: WireClient
    // borrows transport_, Session borrows client_. Declaring transport_ ->
    // client_ -> session_ makes construction (and reverse-order destruction)
    // respect the borrow chain (the "must outlive" contracts in
    // wire_client.hpp / session.hpp).
    ddbsh::wire::SubmitTransport transport_;
    ddbsh::wire::WireClient client_;
    ddbsh::Session session_;
    ddbsh::render::Renderer renderer_;
};

EMSCRIPTEN_BINDINGS(ddbsh) {
    emscripten::value_object<RunResult>("RunResult")
        .field("stdout", &RunResult::stdout_text)
        .field("stderr", &RunResult::stderr_text)
        .field("ok", &RunResult::ok)
        .field("status", &RunResult::status);
    emscripten::class_<DdbshSession>("DdbshSession")
        .constructor<std::string>()
        .function("run", &DdbshSession::run)
        .function("region", &DdbshSession::region)
        .function("endpointOverridden", &DdbshSession::endpoint_overridden);
}

// A trivial entry point. The embind surface is registered by static
// initializers (EMSCRIPTEN_BINDINGS) before main runs, so the module is fully
// usable from JS once the factory promise resolves. The runtime is kept alive
// after main returns (EXIT_RUNTIME defaults off), so the JS side calls
// DdbshSession freely afterward.
int main() { return 0; }

#endif  // defined(__EMSCRIPTEN__)
