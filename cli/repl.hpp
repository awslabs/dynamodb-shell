// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// repl.hpp — the terminal-free CLI driver (isql batch assembly + execute loop).
//
// This is the part of the CLI that owns input assembly and the execute/exit
// policy but has NO terminal dependency (no readline, no AWS SDK): it drives a
// `LineSource` (an abstract one-line-at-a-time seam) through the Sybase-isql
// batch model and hands each assembled batch to the engine `Session`. Splitting
// it out of main.cpp lets the driver be unit-tested offline against a
// `FakeDDBClient`-backed `Session` + an in-memory `LineSource`, closing the
// carried "isql-assembly lacks a focused subprocess test" debt (2h-3).
//
// The readline-backed interactive `StdinLineSource` stays in main.cpp so this
// unit (and the test that links it) remains free of a terminal dependency
// (tenet 3 / D6).

#ifndef DDBSH_CLI_REPL_HPP
#define DDBSH_CLI_REPL_HPP

#include <cstdio>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <string_view>

#include "ddbsh/format.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/retry_event.hpp"
#include "ddbsh/session.hpp"

namespace ddbsh::cli {

/// The CLI's file-writing sink: a FormattingSink (ddbsh/format.hpp) whose
/// writer puts result bytes on `out` and diagnostics on `err`, flushing after
/// every write, so a streaming command's output appears as its data arrives
/// (P12, D-P12-1) and a multi-row INSERT interleaves its written status tokens
/// with a mid-batch error exactly as the original did (AI-4 / D-2c-12). In the
/// default format it writes exactly what the Renderer renders; `--output` and
/// `set output` select another format (P12, D-P12-26). Both streams default to
/// the process's stdout/stderr; tests pass temporary files.
class FileRenderingSink : public ddbsh::render::FormattingSink {
public:
    explicit FileRenderingSink(ddbsh::render::FormatOptions options = {}, std::FILE* out = stdout,
                               std::FILE* err = stderr);
};

/// The message for an output format the CLI does not know, newline included:
///   unknown output format: <id> (expected default, json, csv, table)
std::string unknown_output_format_message(std::string_view id);

/// The message for a JSON flavor the CLI does not know, newline included:
///   unknown JSON flavor: <id> (expected plain, dynamodb)
std::string unknown_json_flavor_message(std::string_view id);

/// The text of one `--show-retries` line (P12, D-P12-20), newline included:
///   ddbsh: Scan throttled (ProvisionedThroughputExceededException); attempt 2 of 3 in 150 ms
///   ddbsh: Query failed (InternalServerError); attempt 3 of 3 in 40 ms
/// "throttled" when the event is a throttle, else "failed"; the error code in
/// parentheses only when there is one; " of N" only when the transport knows
/// its budget; " in N ms" only when it knows the delay. An event with no
/// operation name says "request". CLI presentation policy, not engine logic.
std::string format_retry_line(const ddbsh::RetryEvent& event);

/// The `--show-retries` observer: writes format_retry_line(event) to `err`
/// (default stderr) and flushes, so the line appears while the CLI waits out
/// the backoff. stdout is untouched, so the result bytes are unchanged.
class RetryLineWriter : public ddbsh::RetryObserver {
public:
    explicit RetryLineWriter(std::FILE* err = stderr) : err_(err) {}
    void on_retry(const ddbsh::RetryEvent& event) override;

private:
    std::FILE* err_;
};

// The batch terminator. Either a BLANK line ends the batch, or a word on its
// own line (default `go`) does. A deliberate CLI-layer concept — the engine
// never sees the terminator (statements inside a batch stay ';'-terminated).
struct Terminator {
    bool blank_line = false;  ///< true => a blank line ends the batch
    std::string word = "go";  ///< the terminator word when !blank_line
};

// Resolve the TLS CA-bundle path with AWS-CLI-parity precedence (D1 P7, D-P8-2):
// the explicit `--ca-bundle` flag wins over the `AWS_CA_BUNDLE` environment
// variable, which in turn wins over the resolved profile's `ca_bundle` config
// setting (the third, lowest source added in P8). An empty flag string, an
// empty/unset env value, and an empty/nullopt profile value each do not count
// as "set". Returns the first source that is set, or nullopt when none is.
// Pure logic (no SDK, no terminal), so it is unit-testable in the cli-core
// unit; main.cpp maps the result onto Aws::Client::ClientConfiguration.caFile.
// The profile value is read in the AWS adapter (ddbsh::aws::profile_ca_bundle)
// and passed in here, keeping this function the single, offline-testable owner
// of the precedence contract. Mirrors how the AWS CLI trusts a self-signed
// endpoint (e.g. a local ExtendDB server).
std::optional<std::string> resolve_ca_bundle(const std::optional<std::string>& flag_value,
                                             const char* env_value,
                                             const std::optional<std::string>& profile_value);

// True when `trimmed` (an already-trimmed physical line) is the terminator.
bool is_terminator_line(std::string_view trimmed, const Terminator& term);

// Recognize the CLI-only `set terminator [value]` meta-command at the start of
// a batch. With no value the terminator becomes a blank line; otherwise it
// becomes the given word (surrounding quotes stripped). Returns true if the
// line was the directive and was consumed.
bool try_set_terminator_directive(std::string_view trimmed, Terminator& term);

// Recognize the CLI-only output directives at the start of a batch (P12,
// D-P12-26): `set output <format>` (default, json, csv, table) and
// `set json-flavor <flavor>` (plain, dynamodb). Keywords and values are ASCII
// case-insensitive; surrounding quotes on the value are stripped. Returns true
// if the line was one of them and was consumed. A known value updates
// `options`; an unknown one leaves it unchanged and sets `error` to the
// message (unknown_output_format_message / unknown_json_flavor_message).
bool try_set_output_directive(std::string_view trimmed, ddbsh::render::FormatOptions& options,
                              std::string& error);

// A source of physical input lines (one line, newline stripped), or nullopt at
// end of input. Isolates the CLI's input backends behind one seam.
class LineSource {
public:
    virtual ~LineSource() = default;
    virtual std::optional<std::string> next() = 0;
};

// File / string input: std::getline over any istream (ifstream for -f,
// istringstream for -c, and any istream a test supplies). No prompt, no history.
class StreamLineSource : public LineSource {
public:
    explicit StreamLineSource(std::istream& in) : in_(in) {}
    std::optional<std::string> next() override;

private:
    std::istream& in_;
};

// The result of running one assembled batch through the engine.
struct BatchStatus {
    bool quit = false;     ///< a QUIT statement ran; stop the shell
    bool aborted = false;  ///< abort_on_error fired on a failing statement
    // The LAST EXECUTED statement's error status (nullopt when no command ran,
    // e.g. a pure parse-error or empty batch). This drives the process exit
    // code with the original's last-executed-statement semantics (2h-3): a
    // later success RESETS an earlier failure to success, matching the oracle
    // run loop whose `retval` is overwritten by every pCommand->run() (arch §3).
    std::optional<bool> last_error;
};

// Execute a BATCH of ';'-separated statements through the engine, in order,
// emitting each result FRAGMENT to the caller-supplied `sink` and each rendered
// ERROR string (a failing statement's render_error, and a trailing parse
// error) to the caller-supplied `report_error`. This is the terminal-free,
// output-channel-agnostic core of the batch policy: the FD-vs-string choice is
// the caller's (the CLI's sink writes to stdout/stderr; the web embedder's
// appends to strings). A trailing PARSE error is reported via `report_error`
// but does NOT contribute to the exit status (it never sets `last_error`),
// matching the original run loop. When `abort_on_error` is set, execution stops
// at the first failing statement (that failure is then the last executed →
// nonzero exit). The same batch policy drives both the CLI and the browser
// embedding surface, so it lives in exactly one place (D-P6d-5).
BatchStatus run_batch(ddbsh::Session& session, const ddbsh::render::Renderer& renderer,
                      std::string_view text, bool abort_on_error, ddbsh::OutcomeSink& sink,
                      const std::function<void(const std::string&)>& report_error);

// Execute a BATCH of ';'-separated statements through the engine, in order,
// rendering each result to stdout and each error to stderr. A trailing PARSE
// error is reported to stderr but does NOT contribute to the exit code (it
// never sets `last_error`), matching the original run loop. When
// `abort_on_error` is set, execution stops at the first failing statement (that
// failure is then the last executed → nonzero exit). This is the CLI's thin
// stdout/stderr adapter over run_batch (above).
BatchStatus run_batch_text(ddbsh::Session& session, const ddbsh::render::Renderer& renderer,
                           std::string_view text, bool abort_on_error);

// Drive a LineSource through the isql assembly loop: buffer lines until a
// terminator line (or EOF), flush each batch to the engine. Honors the runtime
// `set terminator` and `set output` / `set json-flavor` meta-commands (at
// batch start), stop-on-quit, and abort-on-error. Returns the exit code: the
// last EXECUTED statement's error status across the whole source (0 if the
// last executed statement succeeded or none ran), with the original's
// last-executed-statement semantics (2h-3). Output goes to stdout/stderr
// through a FileRenderingSink. When `format` is set, the sink starts with
// that selection and `*format` receives the selection in force at the end, so
// the CLI can carry a config file's `set output` into the session (D-P12-26).
int run_source(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, LineSource& src,
               Terminator& term, bool abort_on_error,
               ddbsh::render::FormatOptions* format = nullptr);

// The output-channel-agnostic core of run_source (above): the same isql
// assembly loop, terminator handling, output directives, stop-on-quit,
// abort-on-error, and last-executed-statement exit code, but each batch's
// result fragments go to `sink` and each rendered error (and an output
// directive's unknown-value message) to `report_error` (exactly as
// run_batch). An output directive changes `sink`'s options for the
// statements that follow. The FD-writing overload delegates here with the
// CLI's stdout/stderr sink, so the CLI bytes are unchanged; the browser
// embedding (web/bindings.cpp runScript, P9 D-P9-02-11) buffers them into
// ordered segments instead.
int run_source(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, LineSource& src,
               Terminator& term, bool abort_on_error, ddbsh::render::FormattingSink& sink,
               const std::function<void(const std::string&)>& report_error);

}  // namespace ddbsh::cli

#endif  // DDBSH_CLI_REPL_HPP
