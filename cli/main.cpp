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
//   --ca-bundle <file>
//                trust a custom/self-signed TLS CA bundle, mapped to
//                ClientConfiguration.caFile (AWS-CLI parity, D1). Precedence
//                (D-P8-2): this flag > the AWS_CA_BUNDLE environment variable >
//                the resolved profile's `ca_bundle` config setting > system
//                trust. Needed to reach an endpoint with a self-signed cert
//                (e.g. a local ExtendDB server).

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <getopt.h>

#include <aws/core/client/ClientConfiguration.h>

#include <readline/history.h>
#include <readline/readline.h>

#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/version.hpp"

#include "ddbsh/aws/aws_api_guard.hpp"
#include "ddbsh/aws/aws_ddb_client.hpp"

#include "repl.hpp"

namespace {

// The terminal-free driver (Terminator, LineSource, StreamLineSource,
// run_source) lives in cli/repl.{hpp,cpp} so it can be unit-tested offline;
// only the readline-backed StdinLineSource and process wiring stay here.
using ddbsh::cli::LineSource;
using ddbsh::cli::run_source;
using ddbsh::cli::StreamLineSource;
using ddbsh::cli::Terminator;

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

// Interactive stdin: GNU readline with the region prompt + history. Isolated
// here so the engine stays terminal-free (tenet 3 / D6). The prompt is pulled
// FRESH from a provider before each read, so a CONNECT that changes the
// session's region/endpoint is reflected in the very next prompt (B10 /
// D-2j-2a) — replacing the old startup-snapshot prompt.
// TODO(portability): readline is not native on Windows — a provider/fallback
// behind this same seam lands on the portability ledger.
class StdinLineSource : public LineSource {
public:
    explicit StdinLineSource(std::function<std::string()> prompt_provider)
        : prompt_provider_(std::move(prompt_provider)) {}

    std::optional<std::string> next() override {
        const std::string prompt = prompt_provider_();
        char* raw = readline(prompt.c_str());
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
    std::function<std::string()> prompt_provider_;
};

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
    ddbsh::aws::AwsApiGuard aws_guard;
    install_signal_handlers();

    bool quiet = false;
    bool abort_on_error = false;
    [[maybe_unused]] bool debug = false;
    std::optional<std::string> cmdstring;
    std::optional<std::string> inputfile;
    std::optional<std::string> ca_bundle_flag;  // --ca-bundle <file> (D1)
    Terminator term;                            // default: the word `go`

    // --ca-bundle has no short form (matching the AWS CLI, which exposes only
    // the long flag); give it a val outside the ASCII range so the switch below
    // can distinguish it.
    enum { OPT_CA_BUNDLE = 1000 };
    static const struct option long_opts[] = {
        {"terminator", optional_argument, nullptr, 't'},
        {"ca-bundle", required_argument, nullptr, OPT_CA_BUNDLE},
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
            case OPT_CA_BUNDLE:
                ca_bundle_flag = optarg;
                break;
            default:
                std::fputs(
                    "Usage: ddbsh [-q] [-e] [-d] [-t[TERM]] [--ca-bundle <file>] "
                    "[-c \"<cmds>\" | -f <file>]\n",
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

    // Resolve the TLS CA bundle with AWS-CLI-parity precedence (D1, D-P8-2):
    // the --ca-bundle flag wins over the AWS_CA_BUNDLE environment variable,
    // which in turn wins over the resolved profile's `ca_bundle` config setting
    // (the third, lowest source, added in P8). When set, it is mapped onto
    // ClientConfiguration.caFile so the SDK trusts a custom/self-signed endpoint
    // cert (e.g. a local ExtendDB server). The AWS SDK for C++ (1.11.916) honors
    // NEITHER AWS_CA_BUNDLE NOR a profile `ca_bundle` on its own, so the CLI
    // wires both here: profile_ca_bundle() reads the profile value (it needs
    // InitAPI, already run by AwsApiGuard above), and resolve_ca_bundle() owns
    // the precedence. AwsDDBClient::rebuild() carries caFile across
    // reconnect/CONNECT because it builds every client off the stored
    // base_config.
    const std::optional<std::string> profile_ca = ddbsh::aws::profile_ca_bundle(std::nullopt);
    const std::optional<std::string> ca_bundle =
        ddbsh::cli::resolve_ca_bundle(ca_bundle_flag, std::getenv("AWS_CA_BUNDLE"), profile_ca);

    std::optional<ddbsh::aws::AwsDDBClient> client_storage;
    if (ca_bundle.has_value()) {
        Aws::Client::ClientConfiguration cfg;
        cfg.caFile = Aws::String(ca_bundle->c_str());
        client_storage.emplace(region, endpoint, cfg);
    } else {
        client_storage.emplace(region, endpoint);
    }
    ddbsh::aws::AwsDDBClient& client = *client_storage;
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
    // endpoint override is in effect (arch §4/§11). The prompt is rebuilt from
    // the session's live ConnectionHint before each read, so a CONNECT that
    // changes the region/endpoint is reflected in the next prompt (B10); the
    // `> ` suffix and `(*)` marker are CLI presentation policy (D-2j-2a).
    const auto prompt_provider = [&session]() -> std::string {
        const ddbsh::ConnectionHint hint = session.connection_hint();
        return hint.region + (hint.endpoint_overridden ? " (*)" : "") + "> ";
    };
    const std::string history = history_path();
    if (!history.empty()) {
        read_history(history.c_str());
    }
    StdinLineSource src(prompt_provider);
    int rv = run_source(session, renderer, src, term, abort_on_error);
    if (!history.empty()) {
        write_history(history.c_str());
    }
    return rv;
}
