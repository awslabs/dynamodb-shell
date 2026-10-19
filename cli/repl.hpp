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

#include <istream>
#include <optional>
#include <string>
#include <string_view>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

namespace ddbsh::cli {

// The batch terminator. Either a BLANK line ends the batch, or a word on its
// own line (default `go`) does. A deliberate CLI-layer concept — the engine
// never sees the terminator (statements inside a batch stay ';'-terminated).
struct Terminator {
    bool blank_line = false;  ///< true => a blank line ends the batch
    std::string word = "go";  ///< the terminator word when !blank_line
};

// True when `trimmed` (an already-trimmed physical line) is the terminator.
bool is_terminator_line(std::string_view trimmed, const Terminator& term);

// Recognize the CLI-only `set terminator [value]` meta-command at the start of
// a batch. With no value the terminator becomes a blank line; otherwise it
// becomes the given word (surrounding quotes stripped). Returns true if the
// line was the directive and was consumed.
bool try_set_terminator_directive(std::string_view trimmed, Terminator& term);

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
// rendering each result. A trailing PARSE error is reported to stderr but does
// NOT contribute to the exit code (it never sets `last_error`), matching the
// original run loop. When `abort_on_error` is set, execution stops at the first
// failing statement (that failure is then the last executed → nonzero exit).
BatchStatus run_batch_text(ddbsh::Session& session, const ddbsh::render::Renderer& renderer,
                           std::string_view text, bool abort_on_error);

// Drive a LineSource through the isql assembly loop: buffer lines until a
// terminator line (or EOF), flush each batch to the engine. Honors the runtime
// `set terminator` meta-command (at batch start), stop-on-quit, and
// abort-on-error. Returns the exit code: the last EXECUTED statement's error
// status across the whole source (0 if the last executed statement succeeded or
// none ran), with the original's last-executed-statement semantics (2h-3).
int run_source(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, LineSource& src,
               Terminator& term, bool abort_on_error);

}  // namespace ddbsh::cli

#endif  // DDBSH_CLI_REPL_HPP
