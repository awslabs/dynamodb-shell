// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// main.cpp — the dynamodb-shell CLI (thin line-reader wrapper, tenet 3 / D6).
//
// This wrapper owns ONLY: argument parsing, the prompt, line editing + history
// (GNU readline), config-file loading (later), signal handling (later), AWS SDK
// lifecycle (AwsApiGuard), and constructing the concrete AwsDDBClient. All
// lexing/parsing/execution/formatting lives in the engine library — the CLI
// calls Session::run and writes the Renderer's bytes to its own streams. No
// engine logic lives here.
//
// 2a scope: a REPL and a -c "<cmds>" one-shot that flow through the engine's
// walking-skeleton grammar (SHOW VERSION / QUIT). Full flag parsing (-f, -q,
// -e, -d), config/history files, and signal-to-cancel wiring land in 2f.

#include <cstdio>
#include <cstring>
#include <string>

#include <readline/history.h>
#include <readline/readline.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "aws_api_guard.hpp"
#include "aws_ddb_client.hpp"

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

// Feed one statement's text through the engine and write the result. Returns
// false when the statement asks the shell to quit.
bool run_statement(ddbsh::Session& session, const ddbsh::render::Renderer& renderer,
                   const std::string& text) {
    auto outcome = session.run(text);
    if (!outcome.has_value()) {
        std::fputs(renderer.render_error(outcome.error()).c_str(), stderr);
        return true;
    }
    std::fputs(renderer.render(*outcome).c_str(), stdout);
    // QUIT renders a StatusOutcome{"QUIT"}; detect it to stop the loop.
    if (const auto* status = std::get_if<ddbsh::StatusOutcome>(&*outcome);
        status != nullptr && status->token == "QUIT") {
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    ddbsh::cli::AwsApiGuard aws_guard;

    const std::string region = env_or("AWS_REGION", env_or("AWS_DEFAULT_REGION", "us-east-1"));
    const std::string endpoint = env_or("DDBSH_ENDPOINT_OVERRIDE", "");

    ddbsh::cli::AwsDDBClient client(region, endpoint);
    ddbsh::Session session(ddbsh::SessionConfig{region, endpoint}, client);
    ddbsh::render::Renderer renderer;

    // -c "<cmds>": run a single string then exit (mirrors the original -c).
    if (argc >= 3 && std::strcmp(argv[1], "-c") == 0) {
        run_statement(session, renderer, argv[2]);
        return 0;
    }

    // Interactive REPL. Prompt = "<region>> " with a "(*)" suffix when an
    // endpoint override is in effect (arch §4/§11).
    const std::string prompt = region + (endpoint.empty() ? "" : " (*)") + "> ";
    char* line = nullptr;
    while ((line = readline(prompt.c_str())) != nullptr) {
        if (line[0] != '\0') {
            add_history(line);
        }
        bool keep_going = run_statement(session, renderer, line);
        std::free(line);
        if (!keep_going) {
            break;
        }
    }
    return 0;
}
