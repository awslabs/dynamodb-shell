// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// smoke_gate.cpp — the 2a readiness-gate smoke program (P2a-01 D-Gate).
//
// This is a throwaway diagnostic, NOT a shipped feature. It proves two things
// before the C++23-tree reorg + golden-harness cutover proceeds:
//
//   1. Engine pipeline + version wiring: session.run("SHOW VERSION;") flows
//      text -> parse -> execute -> ExecOutcome and the rendered banner carries
//      the library version. With a DISTINCTIVE version compiled in (see the
//      CMake project VERSION during the gate window), a correct return proves
//      the new C++23 engine is running, not the old C++11 binary.
//   2. Backend reachability: a direct client.list_tables() call against real
//      DynamoDB (us-east-1, standard AWS credential/region resolution) proves
//      the CLI-side AwsDDBClient seam reaches the service.
//
// list_tables() here is called DIRECTLY on the client — it is deliberately NOT
// routed through the grammar/parser. The full SHOW TABLES command stays
// deferred to its proper sub-phase (arch §7).
//
// Build is gated behind -DDDBSH_BUILD_SMOKE_GATE=ON so it never ships in a
// normal build. Once the gate is cleared, this file and its target are removed.

#include <cstdio>
#include <string>

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

}  // namespace

int main() {
    ddbsh::cli::AwsApiGuard aws_guard;

    const std::string region = env_or("AWS_REGION", env_or("AWS_DEFAULT_REGION", "us-east-1"));
    const std::string endpoint = env_or("DDBSH_ENDPOINT_OVERRIDE", "");

    std::printf("=== ddbsh 2a readiness-gate smoke ===\n");
    std::printf("region:   %s\n", region.c_str());
    std::printf("endpoint: %s\n", endpoint.empty() ? "(regional, no override)" : endpoint.c_str());

    ddbsh::cli::AwsDDBClient client(region, endpoint);
    ddbsh::Session session(ddbsh::SessionConfig{region, endpoint}, client);
    ddbsh::render::Renderer renderer;

    // --- Gate 1: engine pipeline + version wiring --------------------------
    std::printf("\n[1] engine: compiled-in version = %s\n", ddbsh::version().c_str());
    auto outcome = session.run("SHOW VERSION;");
    if (!outcome.has_value()) {
        std::fprintf(stderr, "GATE 1 FAILED: %s\n", outcome.error().message.c_str());
        return 1;
    }
    const std::string banner = renderer.render(*outcome);
    std::printf("    session.run(\"SHOW VERSION;\") => %s", banner.c_str());
    if (banner.find(ddbsh::version()) == std::string::npos) {
        std::fprintf(stderr, "GATE 1 FAILED: banner does not carry the library version\n");
        return 1;
    }
    std::printf("    GATE 1 PASS (new C++23 engine pipeline + version wiring)\n");

    // --- Gate 2: real backend reachability ---------------------------------
    auto tables = client.list_tables(ddbsh::ListTablesIn{});
    if (!tables.has_value()) {
        std::fprintf(stderr, "GATE 2 FAILED: list_tables: %s\n", tables.error().message.c_str());
        return 1;
    }
    std::printf("\n[2] client.list_tables() => %zu table(s):\n", tables->table_names.size());
    for (const auto& name : tables->table_names) {
        std::printf("    - %s\n", name.c_str());
    }
    std::printf("    GATE 2 PASS (real DynamoDB reachable via AwsDDBClient seam)\n");

    std::printf("\n=== BOTH GATES PASS ===\n");
    return 0;
}
