// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// ddbshlib_sample.cpp — the LIVE embedding sample (P4d, D28).
//
// A standalone program that embeds the ddbsh library through the new
// ODBC/DB-Library-style embedding API (ddbsh::aws::open -> Database) and runs
// it against REAL DynamoDB. It is a CMake consumer of the installed
// find_package(ddbsh) package: it links `ddbsh::aws` (which pulls
// `ddbsh::ddbsh` + the AWS SDK) and includes only the public headers.
//
// What it shows, end to end, with ONE object and no variant digging:
//   open()        -> a Database (region/credentials/SDK lifecycle all internal)
//   CREATE TABLE  (on-demand, cheapest) -> wait ACTIVE
//   INSERT        a few small rows
//   SELECT        three ways, exercising all three access plans:
//                   full key          -> GetItem   (id = .. AND year = ..)
//                   hash key only     -> Query     (id = ..)
//                   no WHERE          -> Scan       (whole table)
//   DROP TABLE    -> wait gone (self-cleaning, via an RAII scope-guard)
//
// Every Result is checked (no unchecked deref) and the uniquified table is
// dropped on EVERY exit path by a scope-guard, so a failed step never strands
// a live table (D22/D23).
//
// WARNING — THIS IS LIVE. It creates and deletes a REAL DynamoDB table and
// writes/reads REAL items, incurring REAL (tiny) cost. Credentials/region
// resolve through the standard AWS chain (~/.aws, AWS_PROFILE, AWS_REGION);
// default region is us-east-1.

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>

#include "ddbsh/aws/database.hpp"

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : fallback;
}

// A tiny RAII scope-guard: run an arbitrary cleanup on every exit path.
struct ScopeGuard {
    std::function<void()> fn;
    bool active = true;
    ~ScopeGuard() {
        if (active && fn) {
            fn();
        }
    }
    void dismiss() { active = false; }
};

// Run a statement for its byte-exact ddbsh output; return false on error (and
// print the error message to stderr). Checks the Result — no unchecked deref.
bool run(ddbsh::aws::Database& db, const std::string& sql) {
    std::printf("ddbsh> %s\n", sql.c_str());
    ddbsh::Result<ddbsh::Results> r = db.exec(sql);
    if (!r.has_value()) {
        std::fprintf(stderr, "error: %s\n", r.error().message.c_str());
        return false;
    }
    std::fputs(r->render_all().c_str(), stdout);
    if (auto e = r->error()) {
        std::fprintf(stderr, "error: %s\n", e->message.c_str());
        return false;
    }
    std::fflush(stdout);
    return true;
}

}  // namespace

int main() {
    std::puts("=== ddbsh LIVE embedding sample (ddbsh::aws::open, real DynamoDB) ===");
    std::puts("WARNING: creates + deletes a real DynamoDB table; costs real (tiny) money.\n");

    const std::string region = env_or("AWS_REGION", env_or("AWS_DEFAULT_REGION", "us-east-1"));

    // ONE object: region/credentials/SDK lifecycle all handled internally.
    ddbsh::Result<ddbsh::aws::Database> db = ddbsh::aws::open(region);
    if (!db.has_value()) {
        std::fprintf(stderr, "open failed: %s\n", db.error().message.c_str());
        return 1;
    }

    const std::string table = "ddbshlib_sample_" + std::to_string(static_cast<long>(getpid())) +
                              "_" + std::to_string(static_cast<long long>(std::time(nullptr)));
    std::printf("Using region %s, table %s\n\n", region.c_str(), table.c_str());

    bool created = false;
    // Teardown guard: drop the table on EVERY exit path once it was created.
    ScopeGuard teardown{[&] {
        if (created) {
            std::printf("\n[teardown] dropping %s\n", table.c_str());
            (void)db->exec("drop table " + table + " nowait;");
        }
    }};

    if (!run(*db, "create table " + table +
                      " ( id string, year number ) primary key ( id hash, year range )"
                      " billing mode on demand;")) {
        return 1;
    }
    created = true;
    std::puts("");

    if (!run(*db, "insert into " + table +
                      " ( id, year, title ) values ( \"movie\", 2010, \"Inception\" );") ||
        !run(*db, "insert into " + table +
                      " ( id, year, title ) values ( \"movie\", 2014, \"Interstellar\" );") ||
        !run(*db, "insert into " + table +
                      " ( id, year, title ) values ( \"show\", 2016, \"Westworld\" );")) {
        return 1;
    }
    std::puts("");

    // GetItem — full primary key by equality.
    if (!run(*db, "select * from " + table + " where id = \"movie\" and year = 2010;")) {
        return 1;
    }
    std::puts("");

    // Query — hash key only. Shown via the APPLICATION path: typed rows, no
    // variant dig, no Value::Type switch.
    {
        const std::string sql = "select * from " + table + " where id = \"movie\";";
        std::printf("ddbsh> %s   (consumed as typed rows)\n", sql.c_str());
        ddbsh::Result<ddbsh::RowSet> rows = db->query(sql);
        if (!rows.has_value()) {
            std::fprintf(stderr, "error: %s\n", rows.error().message.c_str());
            return 1;
        }
        for (const ddbsh::Row& r : *rows) {
            std::printf(
                "  row: id=%s year=%lld title=%s\n", r.get_string("id").value_or("<none>").c_str(),
                r.get_int("year").value_or(-1), r.get_string("title").value_or("<none>").c_str());
        }
    }
    std::puts("");

    // Scan — no WHERE (byte-exact render path).
    if (!run(*db, "select * from " + table + ";")) {
        return 1;
    }
    std::puts("");

    // DROP — explicit, waits for gone. Dismiss the guard's redundant NOWAIT drop.
    if (!run(*db, "drop table " + table + ";")) {
        return 1;
    }
    teardown.dismiss();

    std::puts("\n=== live sample complete ===");
    return 0;
}
