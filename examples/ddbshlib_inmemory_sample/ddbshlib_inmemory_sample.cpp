// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// ddbshlib_inmemory_sample.cpp — the OFFLINE embedding sample (P4d, D29).
//
// The in-memory counterpart to ddbshlib_sample.cpp: it drives the SAME
// embedding API (ddbsh::Connection / Results / Row / Cell) against a genuine
// in-process backend (ddbsh::testing::MemoryClient) — NO AWS SDK, NO
// credentials, NO network, ZERO cost. It demonstrates that the embedding
// surface is backend-agnostic: swap the client, keep the code. This is also
// how an embedder unit-tests their own ddbsh usage offline.
//
// It links only `ddbsh::ddbsh` (the SDK-free engine); it does not need the AWS
// component. Every Result is checked (no unchecked deref).

#include <cstdio>
#include <string>

#include "ddbsh/connection.hpp"
#include "ddbsh/testing/memory_client.hpp"

namespace {

bool run(ddbsh::Connection& db, const std::string& sql) {
    std::printf("running: %s\n", sql.c_str());
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
    return true;
}

}  // namespace

int main() {
    std::puts("=== ddbsh OFFLINE embedding sample (in-memory client, no AWS) ===\n");

    // A genuine in-memory backend and a Connection over it. Same API as the
    // live sample — only the client differs.
    ddbsh::testing::MemoryClient client;
    ddbsh::Connection db(ddbsh::SessionConfig{"local", ""}, client);

    if (!run(db,
             "create table movies ( id string, year number ) "
             "primary key ( id hash, year range );") ||
        !run(db,
             "insert into movies ( id, year, title ) values ( \"movie\", 2010, \"Inception\" );") ||
        !run(db,
             "insert into movies ( id, year, title ) values ( \"movie\", 2014, \"Interstellar\" "
             ");") ||
        !run(db,
             "insert into movies ( id, year, title ) values ( \"show\", 2016, \"Westworld\" );")) {
        return 1;
    }
    std::puts("");

    // GetItem — full key.
    if (!run(db, "select * from movies where id = \"movie\" and year = 2010;")) {
        return 1;
    }
    std::puts("");

    // Query — hash only, consumed as typed rows.
    {
        ddbsh::Result<ddbsh::RowSet> rows = db.query("select * from movies where id = \"movie\";");
        if (!rows.has_value()) {
            std::fprintf(stderr, "error: %s\n", rows.error().message.c_str());
            return 1;
        }
        std::puts("running: select * from movies where id = \"movie\";   (typed rows)");
        for (const ddbsh::Row& r : *rows) {
            std::printf(
                "  row: id=%s year=%lld title=%s\n", r.get_string("id").value_or("<none>").c_str(),
                r.get_int("year").value_or(-1), r.get_string("title").value_or("<none>").c_str());
        }
    }
    std::puts("");

    // Scan — whole table (byte-exact render path).
    if (!run(db, "select * from movies;")) {
        return 1;
    }

    std::puts("\n=== offline sample complete ===");
    return 0;
}
