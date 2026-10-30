// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// live_demo.cpp — a standalone program that embeds the ddbsh engine library
// and runs it against REAL DynamoDB using the ready-made AWS client component.
//
// This is the LIVE counterpart to examples/embed_demo.cpp. Where embed_demo is
// offline (its own in-memory IDDBClient, zero cost, deterministic), this demo
// shows a third party building their OWN application on `libddbsh` while using
// the real AWS SDK DynamoDB client — the shipping `ddbsh::aws::AwsDDBClient`,
// the same adapter the CLI and the golden suite run on — against real DynamoDB.
//
// It is a CMake consumer of the installed find_package(ddbsh) package: it links
// `ddbsh::aws` (which pulls `ddbsh::ddbsh` + the AWS SDK) and includes only the
// public headers. See examples/live_demo/CMakeLists.txt.
//
// What it demonstrates, end to end against real DynamoDB:
//   CREATE TABLE  (on-demand, cheapest) -> wait ACTIVE
//   INSERT        a few small rows
//   SELECT        three ways, exercising all three access plans:
//                   full key          -> GetItem
//                   hash key only     -> Query
//                   no WHERE          -> Scan
//   DROP TABLE    -> wait gone (self-cleaning)
//
// It shows BOTH result-consumption paths the library offers (D11/D12):
//   1. the "build your own app" path — iterate typed rows with ddbsh::items()
//      and render each cell with ddbsh::render::render_value(), NO variant dig
//      and NO Value::Type switch in the application loop; and
//   2. the "reproduce ddbsh output" path — hand the whole ExecOutcome to
//      ddbsh::render::Renderer for byte-exact ddbsh output.
//
// WARNING — THIS IS LIVE. It creates and deletes a REAL DynamoDB table and
// writes/reads REAL items, incurring REAL (tiny) cost. The table name is
// uniquified per run and the table is dropped on every exit path (a teardown
// scope-guard fires even if a step fails), so it does not strand resources.
// Credentials/region resolve through the standard AWS chain (~/.aws,
// AWS_PROFILE, AWS_REGION); default region is us-east-1.

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

#include "ddbsh/aws/aws_api_guard.hpp"
#include "ddbsh/aws/aws_ddb_client.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

// Execute one statement and print its byte-exact ddbsh output (the Renderer
// path). Returns true on success; on failure prints the error to stderr and
// returns false. This mirrors exactly how the CLI routes success bytes to
// stdout and error bytes to stderr.
bool step(ddbsh::Session& session, const ddbsh::render::Renderer& renderer, std::string_view sql) {
    std::printf("ddbsh> %.*s\n", static_cast<int>(sql.size()), sql.data());
    ddbsh::Result<ddbsh::ExecOutcome> outcome = session.run(sql);
    if (!outcome.has_value()) {
        std::fputs(renderer.render_error(outcome.error()).c_str(), stderr);
        std::fflush(stderr);
        return false;
    }
    std::fputs(renderer.render(*outcome).c_str(), stdout);
    std::fflush(stdout);
    return true;
}

}  // namespace

int main() {
    std::puts("=== ddbsh LIVE embedding demo (real AWS SDK client, real DynamoDB) ===");
    std::puts(
        "WARNING: this creates + deletes a real DynamoDB table and costs real (tiny) money.\n");

    // SDK lifecycle: construct EXACTLY ONE guard, enclosing every AWS client.
    ddbsh::aws::AwsApiGuard aws_guard;

    const std::string region = env_or("AWS_REGION", env_or("AWS_DEFAULT_REGION", "us-east-1"));

    // The ready-made AWS-backed client (default credential chain, no endpoint
    // override). This is all an embedder needs to go live — no hand-written
    // Aws::DynamoDB::Model::* translation.
    ddbsh::aws::AwsDDBClient client(region, "");
    ddbsh::Session session(ddbsh::SessionConfig{region, ""}, client);
    ddbsh::render::Renderer renderer;

    // Uniquify the table name so concurrent/repeat runs never collide and an
    // orphan (should teardown ever be skipped) is easy to spot and sweep.
    const std::string table = "ddbsh_live_demo_" + std::to_string(static_cast<long>(getpid())) +
                              "_" + std::to_string(static_cast<long long>(std::time(nullptr)));
    std::printf("Using region %s, table %s\n\n", region.c_str(), table.c_str());

    bool created = false;
    bool dropped = false;

    // Teardown scope-guard: guarantees the table is removed on EVERY exit path.
    // If the explicit DROP below already ran, this is a no-op; otherwise (a
    // failed step, an early return) it fires a best-effort DROP ... NOWAIT so a
    // live table is never stranded. The library never throws across the seam,
    // so this destructor cannot be bypassed by an exception.
    struct Teardown {
        ddbsh::Session& session;
        const ddbsh::render::Renderer& renderer;
        const std::string& table;
        const bool& created;
        const bool& dropped;
        ~Teardown() {
            if (created && !dropped) {
                std::printf("\n[teardown] dropping %s\n", table.c_str());
                ddbsh::Result<ddbsh::ExecOutcome> o =
                    session.run("drop table " + table + " nowait;");
                if (!o.has_value()) {
                    std::fputs(renderer.render_error(o.error()).c_str(), stderr);
                }
            }
        }
    } teardown{session, renderer, table, created, dropped};

    // 1. CREATE TABLE — on-demand billing (cheapest), composite key, wait ACTIVE
    //    (the engine's default; NOWAIT would skip the wait).
    if (!step(session, renderer,
              "create table " + table +
                  " ( id string, year number ) primary key ( id hash, year range )"
                  " billing mode on demand;")) {
        return 1;
    }
    created = true;
    std::puts("");

    // 2. INSERT a few small rows.
    if (!step(session, renderer,
              "insert into " + table +
                  " ( id, year, title ) values ( \"movie\", 2010, \"Inception\" );") ||
        !step(session, renderer,
              "insert into " + table +
                  " ( id, year, title ) values ( \"movie\", 2014, \"Interstellar\" );") ||
        !step(session, renderer,
              "insert into " + table +
                  " ( id, year, title ) values ( \"show\", 2016, \"Westworld\" );")) {
        return 1;
    }
    std::puts("");

    // 3a. SELECT by full key -> GetItem (Renderer byte-exact path).
    if (!step(session, renderer,
              "select * from " + table + " where id = \"movie\" and year = 2010;")) {
        return 1;
    }
    std::puts("");

    // 3b. SELECT by hash key only -> Query. Shown via the APPLICATION path:
    //     iterate typed rows with ddbsh::items(), print each cell with
    //     ddbsh::render::render_value() — NO variant dig, NO Value::Type switch.
    {
        const std::string sql = "select * from " + table + " where id = \"movie\";";
        std::printf("ddbsh> %s   (consumed as typed rows by the application)\n", sql.c_str());
        ddbsh::Result<ddbsh::ExecOutcome> outcome = session.run(sql);
        if (!outcome.has_value()) {
            std::fputs(renderer.render_error(outcome.error()).c_str(), stderr);
            return 1;
        }
        for (const ddbsh::ItemRow& rw : ddbsh::items(*outcome)) {
            std::string line;
            for (const auto& [name, value] : rw.attributes) {
                if (!line.empty()) {
                    line += ", ";
                }
                line += name + "=" + ddbsh::render::render_value(value);
            }
            std::printf("  row: %s\n", line.c_str());
        }
        // The typed-lookup convenience: one named attribute, no scan by hand.
        for (const ddbsh::ItemRow& rw : ddbsh::items(*outcome)) {
            if (const std::optional<ddbsh::Value> title = rw.get("title"); title.has_value()) {
                std::printf("  title cell: %s\n", ddbsh::render::render_value(*title).c_str());
            }
        }
    }
    std::puts("");

    // 3c. SELECT with no WHERE -> Scan (Renderer byte-exact path).
    if (!step(session, renderer, "select * from " + table + ";")) {
        return 1;
    }
    std::puts("");

    // 4. DROP TABLE -> wait gone (self-cleaning). Mark dropped so the teardown
    //    guard does not issue a redundant NOWAIT drop.
    if (!step(session, renderer, "drop table " + table + ";")) {
        return 1;
    }
    dropped = true;

    std::puts("\n=== live demo complete ===");
    return 0;
}
