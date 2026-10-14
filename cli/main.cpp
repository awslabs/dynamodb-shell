// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// main.cpp — the ddbsh CLI (thin line-reader wrapper, tenet 3 / D6).
//
// This wrapper owns ONLY: argument parsing, the prompt, line editing + history
// (GNU readline), isql-style input assembly (buffer lines until a terminator,
// then hand the whole batch to the engine), config-file loading, signal
// handling, AWS SDK lifecycle (AwsApiGuard), and constructing the concrete
// AwsDDBClient. All lexing/parsing/execution/formatting lives in the engine
// library — the CLI calls Session::parse_batch + Session::execute and writes
// the Renderer's bytes to its own streams. No engine logic lives here.
//
// Input model (human-ratified isql batch model, 2026-10-14):
//   * The engine entry is one-shot over a complete string (Session::parse_batch
//     parses every ';'-terminated statement in order and tolerates a missing
//     trailing ';'; arch §4). The engine never pulls input and carries nothing
//     across calls.
//   * The CLI owns input assembly: it buffers physical lines until it reads a
//     TERMINATOR line, then flushes the accumulated batch to the engine. This
//     is the Sybase isql model (line buffer + a terminator word on its own
//     line). The default terminator is `go`; `-t[STR]` overrides it (`-t` with
//     no argument makes a BLANK line the terminator); and the runtime
//     `set terminator [STR]` meta-command changes it mid-session. EOF is always
//     a terminator (files via -f, the -c string, interactive Ctrl-D) — the
//     buffered batch is flushed regardless.
//   * This isql batch input is a deliberate, documented divergence from the
//     original ddbsh's bare unquoted-';' interactive model (tenet 2/8). It is
//     additive at the CLI layer only; the engine still parses ';'-terminated
//     statements, so golden `.exp` commands (all ';'-terminated, flushed by
//     EOF) are byte-identical. See README "isql-mode input".
//
// Flags (mirroring the original ddbsh getopt surface, arch §3):
//   -c "<cmds>"  run a batch string, then exit (mutually exclusive with -f)
//   -f <file>    run a file, then exit (mutually exclusive with -c)
//   -q           quiet: suppress the startup banner
//   -e           abort on the first error
//   -d           debug (accepted; the engine has no logging subsystem yet)
//   -t[STR] / --terminator[=STR]
//                set the batch terminator (no argument => a blank line)

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <getopt.h>

#include <readline/history.h>
#include <readline/readline.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/version.hpp"

#include "aws_api_guard.hpp"
#include "aws_ddb_client.hpp"

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

// ASCII whitespace trim (locale-independent — the terminator match must not
// depend on the process locale, matching the engine's deterministic parsing).
std::string_view trim(std::string_view s) {
    const auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && is_ws(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && is_ws(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// ASCII case-insensitive equality (the `go` terminator and the `set terminator`
// keywords are matched case-insensitively, per isql convention).
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    const auto lower = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// The batch terminator. Either a BLANK line ends the batch, or a word on its
// own line (default `go`) does. A deliberate CLI-layer concept — the engine
// never sees the terminator (statements inside a batch stay ';'-terminated).
struct Terminator {
    bool blank_line = false;  ///< true => a blank line ends the batch
    std::string word = "go";  ///< the terminator word when !blank_line
};

bool is_terminator_line(std::string_view trimmed, const Terminator& term) {
    if (term.blank_line) {
        return trimmed.empty();
    }
    return !trimmed.empty() && iequals(trimmed, term.word);
}

// Strip a single matched pair of surrounding ASCII quotes (" or ') if present.
std::string_view strip_quotes(std::string_view s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Recognize the CLI-only `set terminator [value]` meta-command. Recognized only
// at the start of a batch (empty buffer), as a standalone line. With no value
// the terminator becomes a blank line; otherwise it becomes the given word
// (surrounding quotes stripped). Returns true if the line was the directive and
// was consumed.
//
// NB (collision): the engine currently has no SET command (DynamoDB ddbsh never
// had one, and the 2b scope excludes it), so intercepting `set terminator` at
// the CLI is unambiguous today. If a future engine SET statement is added, the
// CLI must disambiguate this directive from it.
// TODO(phase-N): revisit if an engine-level SET command is introduced.
bool try_set_terminator_directive(std::string_view trimmed, Terminator& term) {
    // First token must be "set".
    std::size_t p = 0;
    const auto next_token = [&](std::size_t& pos) -> std::string_view {
        while (pos < trimmed.size() && (trimmed[pos] == ' ' || trimmed[pos] == '\t')) {
            ++pos;
        }
        std::size_t start = pos;
        while (pos < trimmed.size() && trimmed[pos] != ' ' && trimmed[pos] != '\t') {
            ++pos;
        }
        return trimmed.substr(start, pos - start);
    };

    if (!iequals(next_token(p), "set")) {
        return false;
    }
    if (!iequals(next_token(p), "terminator")) {
        return false;
    }

    std::string_view rest = trim(trimmed.substr(p));
    rest = strip_quotes(rest);
    if (rest.empty()) {
        term.blank_line = true;
        term.word.clear();
    } else {
        term.blank_line = false;
        term.word = std::string(rest);
    }
    return true;
}

// A source of physical input lines (one line, newline stripped), or nullopt at
// end of input. Isolates the CLI's three input backends behind one seam.
class LineSource {
public:
    virtual ~LineSource() = default;
    virtual std::optional<std::string> next() = 0;
};

// File / string input: std::getline over any istream (ifstream for -f,
// istringstream for -c). No prompt, no history (non-interactive).
class StreamLineSource : public LineSource {
public:
    explicit StreamLineSource(std::istream& in) : in_(in) {}

    std::optional<std::string> next() override {
        std::string line;
        if (!std::getline(in_, line)) {
            return std::nullopt;
        }
        return line;
    }

private:
    std::istream& in_;
};

// Interactive stdin: GNU readline with the region prompt + history. Isolated
// here so the engine stays terminal-free (tenet 3 / D6).
// TODO(portability): readline is not native on Windows — a provider/fallback
// behind this same seam lands on the portability ledger.
class StdinLineSource : public LineSource {
public:
    explicit StdinLineSource(std::string prompt) : prompt_(std::move(prompt)) {}

    std::optional<std::string> next() override {
        char* raw = readline(prompt_.c_str());
        if (raw == nullptr) {
            return std::nullopt;  // EOF (Ctrl-D)
        }
        if (raw[0] != '\0') {
            add_history(raw);
        }
        std::string line(raw);
        std::free(raw);
        return line;
    }

private:
    std::string prompt_;
};

// The result of running one assembled batch through the engine.
struct BatchStatus {
    bool quit = false;       ///< a QUIT statement ran; stop the shell
    bool had_error = false;  ///< at least one statement (or the batch tail) errored
};

// Execute a BATCH of ';'-separated statements through the engine, in order,
// writing each result to the appropriate stream. The engine parses the whole
// string in one call (Session::parse_batch) and tolerates a missing trailing
// ';' (arch §4); the CLI owns the execute loop and the stop-on-quit /
// abort-on-error policy — no lexing/parsing lives here (tenet 3). A trailing
// parse error is rendered to stderr after the statements that parsed ("execute
// what you can"). When abort_on_error is set, execution stops at the first
// failing statement (matching the original run loop's
// `!retval || !abort_on_error()` guard, arch §3).
BatchStatus run_batch_text(ddbsh::Session& session, const ddbsh::render::Renderer& renderer,
                           std::string_view text, bool abort_on_error) {
    BatchStatus status;
    ddbsh::ParsedBatch batch = session.parse_batch(text);
    for (const auto& stmt : batch.statements) {
        auto outcome = session.execute(stmt);
        if (!outcome.has_value()) {
            std::fputs(renderer.render_error(outcome.error()).c_str(), stderr);
            std::fflush(stderr);
            status.had_error = true;
            if (abort_on_error) {
                return status;
            }
            continue;
        }
        std::fputs(renderer.render(*outcome).c_str(), stdout);
        std::fflush(stdout);
        if (stmt.kind() == ddbsh::StatementKind::quit) {
            status.quit = true;
            return status;
        }
    }
    if (batch.error.has_value()) {
        std::fputs(renderer.render_error(*batch.error).c_str(), stderr);
        std::fflush(stderr);
        status.had_error = true;
    }
    return status;
}

// Drive a LineSource through the isql assembly loop: buffer lines until a
// terminator line (or EOF), flush each batch to the engine. Honors the runtime
// `set terminator` meta-command (at batch start), stop-on-quit, and
// abort-on-error. Returns 1 if any error occurred (for the process exit code),
// else 0.
int run_source(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, LineSource& src,
               Terminator& term, bool abort_on_error) {
    std::string buffer;
    bool had_error = false;

    const auto flush = [&](bool& stop) -> void {
        if (buffer.empty()) {
            return;
        }
        BatchStatus st = run_batch_text(session, renderer, buffer, abort_on_error);
        buffer.clear();
        had_error = had_error || st.had_error;
        if (st.quit || (st.had_error && abort_on_error)) {
            stop = true;
        }
    };

    for (std::optional<std::string> line = src.next(); line.has_value(); line = src.next()) {
        std::string_view trimmed = trim(*line);

        // Batch-start meta-command: change the terminator mid-session.
        if (buffer.empty() && try_set_terminator_directive(trimmed, term)) {
            continue;
        }

        if (is_terminator_line(trimmed, term)) {
            bool stop = false;
            flush(stop);
            if (stop) {
                return had_error ? 1 : 0;
            }
            continue;
        }

        if (!buffer.empty()) {
            buffer.push_back('\n');
        }
        buffer += *line;
    }

    // EOF always flushes the remaining buffer (the ratified EOF-is-a-terminator
    // rule): an incomplete tail becomes a syntax error, a complete statement
    // without a trailing ';' is accepted (arch §4).
    bool stop = false;
    flush(stop);
    return had_error ? 1 : 0;
}

// Resolve the config-file path: DDBSH_CONFIG_FILE, else $HOME/.ddbsh_config
// (arch §3). Returns empty when neither can be formed.
std::string config_path() {
    if (const char* explicit_path = std::getenv("DDBSH_CONFIG_FILE");
        explicit_path != nullptr && explicit_path[0] != '\0') {
        return explicit_path;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::string(home) + "/.ddbsh_config";
    }
    return {};
}

// Resolve the history-file path: $HOME/.ddbsh_history (arch §3). Empty when
// $HOME is unset (history is then disabled).
std::string history_path() {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::string(home) + "/.ddbsh_history";
    }
    return {};
}

// A no-op SIGINT handler so Ctrl-C interrupts the current readline() prompt
// without killing the shell (arch §3). Signals are a platform concern.
// TODO(portability): Windows uses a different console-control model; isolate
// behind this same seam when the Windows CLI lands.
extern "C" void handle_sigint(int /*signum*/) {}

void install_signal_handlers() {
#ifndef _WIN32
    std::signal(SIGINT, handle_sigint);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    ddbsh::cli::AwsApiGuard aws_guard;
    install_signal_handlers();

    bool quiet = false;
    bool abort_on_error = false;
    [[maybe_unused]] bool debug = false;
    std::optional<std::string> cmdstring;
    std::optional<std::string> inputfile;
    Terminator term;  // default: the word `go`

    static const struct option long_opts[] = {
        {"terminator", optional_argument, nullptr, 't'},
        {nullptr, 0, nullptr, 0},
    };

    int ch = 0;
    while ((ch = getopt_long(argc, argv, "qc:f:edt::", long_opts, nullptr)) != -1) {
        switch (ch) {
            case 'q':
                quiet = true;
                break;
            case 'e':
                abort_on_error = true;
                break;
            case 'd':
                // Accepted for flag-surface parity with the original. The engine
                // has no logging subsystem yet, so this is currently a no-op.
                // TODO(phase-N): wire to engine logging when it exists.
                debug = true;
                break;
            case 'c':
                cmdstring = optarg;
                break;
            case 'f':
                inputfile = optarg;
                break;
            case 't':
                if (optarg == nullptr || optarg[0] == '\0') {
                    term.blank_line = true;
                    term.word.clear();
                } else {
                    term.blank_line = false;
                    term.word = optarg;
                }
                break;
            default:
                std::fputs("Usage: ddbsh [-q] [-e] [-d] [-t[TERM]] [-c \"<cmds>\" | -f <file>]\n",
                           stderr);
                return 1;
        }
    }

    // -c and -f are mutually exclusive (arch §3).
    if (cmdstring.has_value() && inputfile.has_value()) {
        std::fputs("ddbsh: Cannot specify both -c and -f.\n", stdout);
        return 1;
    }

    const std::string region = env_or("AWS_REGION", env_or("AWS_DEFAULT_REGION", "us-east-1"));
    const std::string endpoint = env_or("DDBSH_ENDPOINT_OVERRIDE", "");

    ddbsh::cli::AwsDDBClient client(region, endpoint);
    ddbsh::Session session(ddbsh::SessionConfig{region, endpoint}, client);
    ddbsh::render::Renderer renderer;

    // Load the config file first (arch §3): run it with abort-on-error TRUE so a
    // bad config line stops startup (the original __runfile exits(1)). A QUIT in
    // the config ends config processing only; it does not quit the shell (the
    // original resets m_quit at the top of each run()).
    if (const std::string cfg = config_path(); !cfg.empty()) {
        std::ifstream in(cfg);
        if (in) {
            Terminator cfg_term;  // config uses ';' statements + EOF (default `go`)
            StreamLineSource src(in);
            if (run_source(session, renderer, src, cfg_term, /*abort_on_error=*/true) != 0) {
                return 1;
            }
        }
    }

    // Startup banner (suppressed by -q, which the golden harness passes).
    if (!quiet) {
        std::printf("ddbsh - version %s\n", ddbsh::version().c_str());
        std::fflush(stdout);
    }

    // -c "<cmds>": run a batch string, then exit.
    if (cmdstring.has_value()) {
        std::istringstream in(*cmdstring);
        StreamLineSource src(in);
        return run_source(session, renderer, src, term, abort_on_error);
    }

    // -f <file>: run a file, then exit.
    if (inputfile.has_value()) {
        std::ifstream in(*inputfile);
        if (!in) {
            std::perror(inputfile->c_str());
            return 1;
        }
        StreamLineSource src(in);
        return run_source(session, renderer, src, term, abort_on_error);
    }

    // Interactive REPL. Prompt = "<region>> " with a "(*)" suffix when an
    // endpoint override is in effect (arch §4/§11).
    // TODO(phase-N): the prompt is fixed at startup; reflecting a region change
    // from a CONNECT (and reconnecting the client) is deferred to the
    // reconnection round.
    const std::string prompt = region + (endpoint.empty() ? "" : " (*)") + "> ";
    const std::string history = history_path();
    if (!history.empty()) {
        read_history(history.c_str());
    }
    StdinLineSource src(prompt);
    int rv = run_source(session, renderer, src, term, abort_on_error);
    if (!history.empty()) {
        write_history(history.c_str());
    }
    return rv;
}
