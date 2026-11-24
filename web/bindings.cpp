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
// Three entry points (D-P9-02-11; P12, D-P12-17):
//   run(text)                       one submission through run_batch (no
//                                   terminator lines, no stop-on-quit across
//                                   submissions), buffered.
//   runStreaming(text, options, onEvent)
//                                   the UI path (P12): the same run_batch
//                                   submission as run(), but each rendered
//                                   piece of output goes to onEvent as soon as
//                                   the engine emits it, so the page can paint
//                                   it while the next request is in flight.
//                                   `options` is runScript's, plus the
//                                   web-only output "html", which sends rows
//                                   as structured events (D-P12-25).
//   runScript(text, abortOnError, options)
//                                   the CLI `-c` / `-f` path: run_source over
//                                   the text, exactly as the CLI drives it
//                                   (terminator lines, `set output`,
//                                   stop-on-quit, -e). `options` is
//                                   {output, jsonFlavor}, the CLI's --output
//                                   and --json-flavor (P12). The golden gate
//                                   (web/ddbsh-node.mjs) uses it.
// run and runScript return RunResult.segments: the rendered stdout/stderr text
// in emission order, so a consumer that merges the streams (the golden
// runner's 2>&1) sees the same interleaving as the CLI. runStreaming delivers
// the same text, in the same order, as events instead.
//
// One free function: version() returns ddbsh::version() (CMake
// project(VERSION), the string SHOW VERSION prints), which the page shows in
// its header (P10, D-P10-14).
//
// WASM-only: this TU is compiled solely for the Emscripten web target.

#if defined(__EMSCRIPTEN__)

#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "ddbsh/config.hpp"
#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/format.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/version.hpp"
#include "ddbsh/wire/wire_client.hpp"
#include "js_sdk_transport.hpp"
#include "repl.hpp"

namespace {

// The JS-visible result of one run / runScript / runStreaming. `segments` is a
// JS array of {stream: "stdout"|"stderr", text} in emission order (adjacent
// same-stream text coalesced); `stdout`/`stderr` are the per-stream
// concatenations (runStreaming leaves all three empty: its text went to the
// event callback); `ok`/`status` follow the CLI's last-executed-statement exit
// semantics.
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

// Buffers each emitted fragment into a RunResult: a FormattingSink (the same
// one the CLI's sink is) whose writer appends to the result's segments, so the
// bytes and their stdout/stderr order are the CLI's in every output format. A
// rejected CONNECT's notice follows that statement's output.
class BufferingSink : public ddbsh::render::FormattingSink {
public:
    BufferingSink(ddbsh::render::FormatOptions options, WebWireClient& client, RunResult& result)
        : FormattingSink(options,
                         [&result](ddbsh::render::Stream stream, std::string_view text) {
                             result.append(
                                 stream == ddbsh::render::Stream::out ? "stdout" : "stderr",
                                 std::string(text));
                         }),
          client_(client),
          result_(result) {}

    void emit(const ddbsh::ExecOutcome& fragment) override {
        FormattingSink::emit(fragment);
        result_.append("stderr", client_.take_notice());
    }

private:
    WebWireClient& client_;
    RunResult& result_;
};

// One output selection read from a JS `options` object ({output, jsonFlavor},
// P12 D-P12-26). `html` is set for the web-only "html" id (D-P12-25), which
// only runStreaming accepts; `format` then carries just the JSON flavor (the
// flavor of nested values in the cells).
struct WebFormat {
    ddbsh::render::FormatOptions format;
    bool html = false;
};

// ASCII case-insensitive equality, as parse_output_format compares ids.
bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

// The text that names an option value in an unknown-value message. A string
// is named as itself. Any other value (a number, an object, ...) is an
// unknown value too (P12 N-S5-1), named by its JavaScript type in angle
// brackets, for example `<number>`. The type is read with `typeof`, which
// cannot throw; converting the value itself (String(v)) could run a throwing
// toString while the engine is on the stack.
std::string option_value_name(const emscripten::val& value) {
    if (value.isString()) {
        return value.as<std::string>();
    }
    return "<" + value.typeOf().as<std::string>() + ">";
}

// runStreaming's message for an unknown output id. It lists `html`, which
// only runStreaming accepts (P12 N-S5-2). runScript keeps the CLI's message
// (unknown_output_format_message), which the shim parity and the
// ifmt-bad-output golden pin.
std::string unknown_web_output_format_message(std::string_view id) {
    return "unknown output format: " + std::string(id) +
           " (expected default, json, csv, table, html)\n";
}

// Read `options` into `out`. Absent (or undefined/null) fields keep the
// default. `allow_html` admits the "html" id (runStreaming only; runScript
// rejects it as the CLI does) and selects the message that lists it. Returns
// the message for an unknown value (a non-string value is unknown), or ""
// when every value is known.
std::string read_format_options(const emscripten::val& options, WebFormat& out, bool allow_html) {
    if (options.isUndefined() || options.isNull()) {
        return {};
    }
    const emscripten::val output = options["output"];
    if (!output.isUndefined() && !output.isNull()) {
        const std::string id = option_value_name(output);
        if (!output.isString()) {
            return allow_html ? unknown_web_output_format_message(id)
                              : ddbsh::cli::unknown_output_format_message(id);
        }
        if (allow_html && iequals_ascii(id, "html")) {
            out.html = true;
        } else if (auto f = ddbsh::render::parse_output_format(id); f.has_value()) {
            out.format.format = *f;
        } else {
            return allow_html ? unknown_web_output_format_message(id)
                              : ddbsh::cli::unknown_output_format_message(id);
        }
    }
    const emscripten::val flavor = options["jsonFlavor"];
    if (!flavor.isUndefined() && !flavor.isNull()) {
        const std::string id = option_value_name(flavor);
        if (!flavor.isString()) {
            return ddbsh::cli::unknown_json_flavor_message(id);
        }
        auto f = ddbsh::render::parse_json_flavor(id);
        if (!f.has_value()) {
            return ddbsh::cli::unknown_json_flavor_message(id);
        }
        out.format.json_flavor = *f;
    }
    return {};
}

// runStreaming's JavaScript callback (P12, D-P12-17). It runs synchronously
// inside emit(), while the engine is on the stack; it must not throw (the page
// passes app-core's guardedEventSink) and must not call back into the module
// or the session.
struct EventChannel {
    emscripten::val on_event;

    // One {type: "text", stream, text} event; empty text sends nothing.
    void text(const char* stream, std::string_view text) const {
        if (text.empty()) {
            return;
        }
        emscripten::val ev = emscripten::val::object();
        ev.set("type", std::string("text"));
        ev.set("stream", std::string(stream));
        ev.set("text", std::string(text));
        on_event(ev);
    }

    void send(const emscripten::val& ev) const { on_event(ev); }
};

// runStreaming in `default`, `json`, `csv` and `table`: a FormattingSink (the
// one the CLI and runScript use) whose writer sends each piece of output as
// one text event, so the events of a run concatenate to runScript's bytes.
// A rejected CONNECT's notice follows that statement's output, as in
// BufferingSink.
class StreamingSink : public ddbsh::render::FormattingSink {
public:
    StreamingSink(ddbsh::render::FormatOptions options, WebWireClient& client,
                  const EventChannel& channel)
        : FormattingSink(options,
                         [&channel](ddbsh::render::Stream stream, std::string_view text) {
                             channel.text(
                                 stream == ddbsh::render::Stream::out ? "stdout" : "stderr", text);
                         }),
          client_(client),
          channel_(channel) {}

    void emit(const ddbsh::ExecOutcome& fragment) override {
        FormattingSink::emit(fragment);
        channel_.text("stderr", client_.take_notice());
    }

private:
    WebWireClient& client_;
    const EventChannel& channel_;
};

// runStreaming in the web-only `html` format (P12, D-P12-25, design §4.7).
// The library emits no HTML: this sink sends the rows of each result set as
// structured events and the page builds the table from them with
// createElement/textContent. It is a ResultSetSink, so its result sets open
// and close exactly where FormattingSink's JSON arrays do:
//   {type: "rows", columns, newColumnsFrom, rows, groupEnd}
//       one per RowsPart that brings something: the first part of a set
//       (even with no rows: an empty SELECT still shows its header), and any
//       later part with rows or new columns (a later empty part, such as the
//       capacity-only fragment that ends a SELECT or a page end a LIKE filter
//       emptied, sends nothing). `columns` is the set's full column list so
//       far (ColumnTracker); `newColumnsFrom` the index of the first column
//       this event added (== columns.length when none). `rows` holds one
//       array per row, one cell per column in `columns`:
//       {t: format_cell text (not escaped; the page sets it as textContent),
//        n: the value is a number (N), m: the row has no such attribute}.
//       `groupEnd` is RowsPart::group_end (an API page ended here).
//   {type: "end"}
//       the result set closed (before the text that follows it, and at the
//       end of the statement, so before a failing statement's error).
// Everything else (status tokens, DESCRIBE, EXPLAIN, capacity lines) arrives
// as text events with the Renderer's own bytes, as in `default`.
class HtmlEventSink : public ddbsh::render::ResultSetSink {
public:
    HtmlEventSink(ddbsh::render::JsonFlavor flavor, WebWireClient& client,
                  const EventChannel& channel)
        : flavor_(flavor), client_(client), channel_(channel) {}

    void emit(const ddbsh::ExecOutcome& fragment) override {
        ResultSetSink::emit(fragment);
        channel_.text("stderr", client_.take_notice());
    }

private:
    void on_open() override {
        tracker_.clear();
        announced_ = false;
    }

    void on_rows(const ddbsh::render::RowsPart& part) override {
        const std::size_t from = tracker_.add(part);
        const std::vector<std::string>& columns = tracker_.columns();
        // A part with no rows and no new column carries nothing for the page
        // once the set has had its first event (the capacity-only fragment
        // that ends a SELECT, a page end a LIKE filter emptied). The first
        // event of a set is always sent, so an empty result still shows its
        // header.
        if (announced_ && part.rows.empty() && from == columns.size()) {
            return;
        }
        announced_ = true;
        emscripten::val cols = emscripten::val::array();
        for (const auto& c : columns) {
            cols.call<void>("push", c);
        }
        emscripten::val rows = emscripten::val::array();
        for (const auto& row : part.rows) {
            emscripten::val cells = emscripten::val::array();
            for (const auto& c : columns) {
                cells.call<void>("push", cell(row.attributes, c));
            }
            rows.call<void>("push", cells);
        }
        emscripten::val ev = emscripten::val::object();
        ev.set("type", std::string("rows"));
        ev.set("columns", cols);
        ev.set("newColumnsFrom", static_cast<double>(from));
        ev.set("rows", rows);
        ev.set("groupEnd", part.group_end);
        channel_.send(ev);
    }

    void on_close() override {
        tracker_.clear();
        emscripten::val ev = emscripten::val::object();
        ev.set("type", std::string("end"));
        channel_.send(ev);
    }

    void on_text(const ddbsh::render::TextPart& part) override {
        channel_.text("stdout", part.out);
        channel_.text("stderr", part.err);
    }

    // One {t, n, m} cell: the attribute `name` of `attrs`, or a missing cell.
    emscripten::val cell(const ddbsh::ValueMap& attrs, const std::string& name) const {
        emscripten::val c = emscripten::val::object();
        for (const auto& [key, value] : attrs) {
            if (key == name) {
                c.set("t", ddbsh::render::format_cell(value, flavor_));
                c.set("n", value.type == ddbsh::Value::Type::N);
                c.set("m", false);
                return c;
            }
        }
        c.set("t", std::string());
        c.set("n", false);
        c.set("m", true);
        return c;
    }

    ddbsh::render::JsonFlavor flavor_;
    WebWireClient& client_;
    const EventChannel& channel_;
    ddbsh::render::ColumnTracker tracker_;
    bool announced_ = false;  ///< the open set has sent its first rows event
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
        BufferingSink sink({}, client_, result);
        ddbsh::cli::BatchStatus st = ddbsh::cli::run_batch(
            session_, renderer_, text, /*abort_on_error=*/false, sink,
            [&](const std::string& rendered) { result.append("stderr", rendered); });
        const bool failed = st.last_error.value_or(false);
        result.ok = !failed;
        result.status = failed ? 1 : 0;
        return result;
    }

    // The streaming UI path (P12, D-P12-17): run() with each piece of output
    // delivered to `on_event` as it is emitted, a failing statement's error as
    // a stderr event after that statement's output (and after the `end` of its
    // result set), and a trailing parse error last. `options` is
    // {output, jsonFlavor} as in runScript, plus the web-only output "html"
    // (D-P12-25; see HtmlEventSink). In `default`, `json`, `csv` and `table`
    // the text events concatenate to the bytes runScript returns for the same
    // text and options. Returns {ok, status} with empty text fields and no
    // segments, so nothing is delivered twice. A non-function `on_event` runs
    // nothing and returns status 1 with the reason in `stderr`; an unknown
    // option value (a non-string value included) runs nothing and returns
    // status 1 with its message as a stderr event. For `output`, that message
    // lists `html` (unknown_web_output_format_message).
    RunResult run_streaming(const std::string& text, emscripten::val options,
                            emscripten::val on_event) {
        RunResult result;
        if (on_event.typeOf().as<std::string>() != "function") {
            result.stderr_text = "runStreaming: onEvent is not a function\n";
            result.ok = false;
            result.status = 1;
            return result;
        }
        const EventChannel channel{std::move(on_event)};
        WebFormat format;
        if (std::string error = read_format_options(options, format, /*allow_html=*/true);
            !error.empty()) {
            channel.text("stderr", error);
            result.ok = false;
            result.status = 1;
            return result;
        }
        const auto report = [&channel](const std::string& rendered) {
            channel.text("stderr", rendered);
        };
        ddbsh::cli::BatchStatus st;
        if (format.html) {
            HtmlEventSink sink(format.format.json_flavor, client_, channel);
            st = ddbsh::cli::run_batch(session_, renderer_, text, /*abort_on_error=*/false, sink,
                                       report);
        } else {
            StreamingSink sink(format.format, client_, channel);
            st = ddbsh::cli::run_batch(session_, renderer_, text, /*abort_on_error=*/false, sink,
                                       report);
        }
        const bool failed = st.last_error.value_or(false);
        result.ok = !failed;
        result.status = failed ? 1 : 0;
        return result;
    }

    // The CLI `-c` / `-f` path (run_source): status is the CLI's exit code.
    // `options` ({output, jsonFlavor}) is the CLI's --output / --json-flavor
    // (P12, D-P12-26); `set output` lines in the text change it as in the CLI.
    // An unknown value (a non-string value included) runs nothing and returns
    // status 1 with the CLI's message in stderr.
    RunResult run_script(const std::string& text, bool abort_on_error, emscripten::val options) {
        RunResult result;
        WebFormat format;
        if (std::string error = read_format_options(options, format, /*allow_html=*/false);
            !error.empty()) {
            result.append("stderr", error);
            result.ok = false;
            result.status = 1;
            return result;
        }
        BufferingSink sink(format.format, client_, result);
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
        .function("runStreaming", &DdbshSession::run_streaming)
        .function("runScript", &DdbshSession::run_script)
        .function("region", &DdbshSession::region)
        .function("endpointOverridden", &DdbshSession::endpoint_overridden);
    emscripten::function("version", &ddbsh::version);
}

// A trivial entry point. The embind surface is registered by static
// initializers (EMSCRIPTEN_BINDINGS) before main runs, so the module is fully
// usable from JS once the factory promise resolves. The runtime is kept alive
// after main returns (EXIT_RUNTIME defaults off).
int main() {
    return 0;
}

#endif  // defined(__EMSCRIPTEN__)
