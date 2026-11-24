// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// repl.cpp — the terminal-free CLI driver (see repl.hpp).

#include "repl.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ddbsh::cli {

namespace {

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

// Strip a single matched pair of surrounding ASCII quotes (" or ') if present.
std::string_view strip_quotes(std::string_view s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// A failing statement's error (and a trailing parse error) go to stderr,
// flushed so a mid-batch error interleaves with already-written stdout tokens.
void report_error_to_stderr(const std::string& rendered) {
    std::fputs(rendered.c_str(), stderr);
    std::fflush(stderr);
}

// Write `text` to `f` and flush, so each piece of output is on the terminal
// (or pipe) before the engine makes its next API call (P12, D-P12-1). Like the
// fputs this replaces (and the original's printf), it stops at an embedded
// NUL byte, so the default output keeps its exact bytes.
void write_and_flush(std::FILE* f, std::string_view text) {
    std::fwrite(text.data(), 1, std::min(text.size(), text.find('\0')), f);
    std::fflush(f);
}

}  // namespace

FileRenderingSink::FileRenderingSink(ddbsh::render::FormatOptions options, std::FILE* out,
                                     std::FILE* err)
    : FormattingSink(options, [out, err](ddbsh::render::Stream stream, std::string_view text) {
          write_and_flush(stream == ddbsh::render::Stream::out ? out : err, text);
      }) {}

std::string unknown_output_format_message(std::string_view id) {
    return "unknown output format: " + std::string(id) + " (expected default, json, csv, table)\n";
}

std::string unknown_json_flavor_message(std::string_view id) {
    return "unknown JSON flavor: " + std::string(id) + " (expected plain, dynamodb)\n";
}

std::string format_retry_line(const ddbsh::RetryEvent& event) {
    std::string line = "ddbsh: ";
    line += event.operation.empty() ? std::string("request") : event.operation;
    line += event.throttled ? " throttled" : " failed";
    if (!event.error_code.empty()) {
        line += " (" + event.error_code + ")";
    }
    line += "; attempt " + std::to_string(event.attempt);
    if (event.max_attempts > 0) {
        line += " of " + std::to_string(event.max_attempts);
    }
    if (event.delay.has_value()) {
        line += " in " + std::to_string(event.delay->count()) + " ms";
    }
    line += "\n";
    return line;
}

void RetryLineWriter::on_retry(const ddbsh::RetryEvent& event) {
    std::fputs(format_retry_line(event).c_str(), err_);
    std::fflush(err_);
}

std::optional<std::string> resolve_ca_bundle(const std::optional<std::string>& flag_value,
                                             const char* env_value,
                                             const std::optional<std::string>& profile_value) {
    // Precedence (D-P8-2): flag > env > profile > none. An empty value from any
    // source does not count as "set".
    if (flag_value.has_value() && !flag_value->empty()) {
        return *flag_value;
    }
    if (env_value != nullptr && env_value[0] != '\0') {
        return std::string(env_value);
    }
    if (profile_value.has_value() && !profile_value->empty()) {
        return *profile_value;
    }
    return std::nullopt;
}

bool is_terminator_line(std::string_view trimmed, const Terminator& term) {
    if (term.blank_line) {
        return trimmed.empty();
    }
    return !trimmed.empty() && iequals(trimmed, term.word);
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

// Recognize `set output <format>` / `set json-flavor <flavor>` (P12,
// D-P12-26), like `set terminator`: only at the start of a batch, as a
// standalone line. The same SET-collision note applies (see above).
bool try_set_output_directive(std::string_view trimmed, ddbsh::render::FormatOptions& options,
                              std::string& error) {
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
    const std::string_view what = next_token(p);
    const bool output = iequals(what, "output");
    if (!output && !iequals(what, "json-flavor")) {
        return false;
    }

    const std::string_view value = strip_quotes(trim(trimmed.substr(p)));
    error.clear();
    if (output) {
        if (auto f = ddbsh::render::parse_output_format(value); f.has_value()) {
            options.format = *f;
        } else {
            error = unknown_output_format_message(value);
        }
    } else {
        if (auto f = ddbsh::render::parse_json_flavor(value); f.has_value()) {
            options.json_flavor = *f;
        } else {
            error = unknown_json_flavor_message(value);
        }
    }
    return true;
}

std::optional<std::string> StreamLineSource::next() {
    std::string line;
    if (!std::getline(in_, line)) {
        return std::nullopt;
    }
    return line;
}

BatchStatus run_batch(ddbsh::Session& session, const ddbsh::render::Renderer& renderer,
                      std::string_view text, bool abort_on_error, ddbsh::OutcomeSink& sink,
                      const std::function<void(const std::string&)>& report_error) {
    BatchStatus status;
    ddbsh::ParsedBatch batch = session.parse_batch(text);
    for (const auto& stmt : batch.statements) {
        auto outcome = session.execute(stmt, sink);
        if (!outcome.has_value()) {
            report_error(renderer.render_error(outcome.error()));
            // This failing statement is the most-recently executed one.
            status.last_error = true;
            if (abort_on_error) {
                status.aborted = true;
                return status;
            }
            continue;
        }
        // A successful command run() RESETS the last-executed status to OK,
        // matching the oracle's retval overwrite (arch §3).
        status.last_error = false;
        if (stmt.kind() == ddbsh::StatementKind::quit) {
            status.quit = true;
            return status;
        }
    }
    if (batch.error.has_value()) {
        // A PARSE error is reported to the error channel but does NOT set the
        // process error exit, matching the original's run loop (ddbsh.cpp
        // CDDBSh::run): `retval` is updated ONLY by `pCommand->run()`, so a
        // yyparse failure leaves it untouched (0 for a pure parse error) and
        // the shell exits 0. Only a command's run() failure (handled above)
        // contributes to the nonzero exit code. This is the behavior the golden
        // suite pins — e.g. ddbsh-basic-test-9 / ddbsh-cross-account-test-9,
        // whose mutual-exclusion semantic error must still exit 0 (the oracle
        // does). Abort-on-error likewise keys off run() failures, not parse
        // failures (a config-file parse error does not abort startup in the
        // original either). Crucially, it also leaves `last_error` UNTOUCHED, so
        // a trailing parse error does not disturb the last executed statement's
        // contribution to the exit code.
        report_error(renderer.render_error(*batch.error));
    }
    return status;
}

BatchStatus run_batch_text(ddbsh::Session& session, const ddbsh::render::Renderer& renderer,
                           std::string_view text, bool abort_on_error) {
    FileRenderingSink sink;
    return run_batch(session, renderer, text, abort_on_error, sink, report_error_to_stderr);
}

int run_source(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, LineSource& src,
               Terminator& term, bool abort_on_error, ddbsh::render::FormatOptions* format) {
    FileRenderingSink sink(format != nullptr ? *format : ddbsh::render::FormatOptions{});
    const int rc =
        run_source(session, renderer, src, term, abort_on_error, sink, report_error_to_stderr);
    if (format != nullptr) {
        *format = sink.options();
    }
    return rc;
}

int run_source(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, LineSource& src,
               Terminator& term, bool abort_on_error, ddbsh::render::FormattingSink& sink,
               const std::function<void(const std::string&)>& report_error) {
    std::string buffer;
    // The exit code tracks the LAST EXECUTED statement across the WHOLE source
    // (the original's single run loop spans every statement, not a batch), with
    // last-executed-statement semantics: every executed command overwrites it,
    // so a success after a failure exits 0 (2h-3). nullopt => nothing executed
    // (only parse errors / empty input) => exit 0.
    std::optional<bool> last_error;

    const auto flush = [&](bool& stop) -> void {
        if (buffer.empty()) {
            return;
        }
        BatchStatus st = run_batch(session, renderer, buffer, abort_on_error, sink, report_error);
        buffer.clear();
        if (st.last_error.has_value()) {
            last_error = st.last_error;
        }
        if (st.quit || st.aborted) {
            stop = true;
        }
    };

    for (std::optional<std::string> line = src.next(); line.has_value(); line = src.next()) {
        std::string_view trimmed = trim(*line);

        // Batch-start meta-commands: change the terminator, or the output
        // format, mid-session. An unknown format is reported like a parse
        // error: on the error channel, without touching the exit code.
        if (buffer.empty()) {
            if (try_set_terminator_directive(trimmed, term)) {
                continue;
            }
            ddbsh::render::FormatOptions options = sink.options();
            std::string error;
            if (try_set_output_directive(trimmed, options, error)) {
                if (error.empty()) {
                    sink.set_options(options);
                } else {
                    report_error(error);
                }
                continue;
            }
        }

        if (is_terminator_line(trimmed, term)) {
            bool stop = false;
            flush(stop);
            if (stop) {
                return last_error.value_or(false) ? 1 : 0;
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
    return last_error.value_or(false) ? 1 : 0;
}

}  // namespace ddbsh::cli
