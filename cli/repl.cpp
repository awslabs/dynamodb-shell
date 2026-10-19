// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// repl.cpp — the terminal-free CLI driver (see repl.hpp).

#include "repl.hpp"

#include <cstdio>

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

}  // namespace

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

std::optional<std::string> StreamLineSource::next() {
    std::string line;
    if (!std::getline(in_, line)) {
        return std::nullopt;
    }
    return line;
}

BatchStatus run_batch_text(ddbsh::Session& session, const ddbsh::render::Renderer& renderer,
                           std::string_view text, bool abort_on_error) {
    // Render each outcome fragment to stdout as the engine emits it, so a
    // multi-row INSERT (or any future streaming write) interleaves its
    // already-written status tokens with a mid-batch error exactly as the
    // original did (AI-4 / D-2c-12).
    struct RenderingSink : ddbsh::OutcomeSink {
        const ddbsh::render::Renderer& renderer;
        explicit RenderingSink(const ddbsh::render::Renderer& r) : renderer(r) {}
        void emit(const ddbsh::ExecOutcome& fragment) override {
            // stdout gets the result bytes; stderr gets any diagnostics the
            // outcome carries (a transaction's non-reducible-SELECT message,
            // D-2d-6). Both are flushed so a mid-batch error interleaves
            // correctly (AI-4 / D-2c-12).
            std::fputs(renderer.render(fragment).c_str(), stdout);
            std::fflush(stdout);
            std::string diag = renderer.render_stderr(fragment);
            if (!diag.empty()) {
                std::fputs(diag.c_str(), stderr);
                std::fflush(stderr);
            }
        }
    };

    BatchStatus status;
    ddbsh::ParsedBatch batch = session.parse_batch(text);
    for (const auto& stmt : batch.statements) {
        RenderingSink sink(renderer);
        auto outcome = session.execute(stmt, sink);
        if (!outcome.has_value()) {
            std::fputs(renderer.render_error(outcome.error()).c_str(), stderr);
            std::fflush(stderr);
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
        // A PARSE error is reported to stderr but does NOT set the process
        // error exit, matching the original's run loop (ddbsh.cpp CDDBSh::run):
        // `retval` is updated ONLY by `pCommand->run()`, so a yyparse failure
        // leaves it untouched (0 for a pure parse error) and the shell exits 0.
        // Only a command's run() failure (handled above) contributes to the
        // nonzero exit code. This is the behavior the golden suite pins — e.g.
        // ddbsh-basic-test-9 / ddbsh-cross-account-test-9, whose mutual-exclusion
        // semantic error must still exit 0 (the oracle does). Abort-on-error
        // likewise keys off run() failures, not parse failures (a config-file
        // parse error does not abort startup in the original either). Crucially,
        // it also leaves `last_error` UNTOUCHED, so a trailing parse error does
        // not disturb the last executed statement's contribution to the exit
        // code.
        std::fputs(renderer.render_error(*batch.error).c_str(), stderr);
        std::fflush(stderr);
    }
    return status;
}

int run_source(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, LineSource& src,
               Terminator& term, bool abort_on_error) {
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
        BatchStatus st = run_batch_text(session, renderer, buffer, abort_on_error);
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

        // Batch-start meta-command: change the terminator mid-session.
        if (buffer.empty() && try_set_terminator_directive(trimmed, term)) {
            continue;
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
