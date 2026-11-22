// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// ddbshlib_features_sample.cpp — the Phase-5 FEATURE embedding sample (D9).
//
// A worked tour of the embedding capabilities added in Phase 5, all driven
// OFFLINE against the in-memory ddbsh::testing::MemoryClient — NO AWS SDK, NO
// credentials, NO network, ZERO cost. It links only `ddbsh::ddbsh` (the
// SDK-free engine). Every feature here is backend-agnostic: the same code runs
// against real DynamoDB by opening the Connection over the AWS client instead
// (see examples/ddbshlib_sample for the live counterpart).
//
// The sample is organized into four clearly-labeled sections, each
// demonstrating one Phase-5 capability:
//
//   1. Prepared / parameterized statements — prepare once, bind positional `?`
//      parameters, execute, then RE-execute with different bindings without
//      re-parsing (the ODBC / Sybase DB-Library shape).
//   2. Typed set / binary bind — bind SS/NS/BS/B values (which have no ddbsh
//      literal syntax) through the prepared-statement path and read them back.
//   3. Lazy streaming — set a small page size on the backend and drain a
//      multi-row SELECT row-by-row through Results::next_row(), so only one
//      page is resident in memory at a time.
//   4. Transactions — a BEGIN … COMMIT write transaction (atomic) and a read
//      transaction that reduces to TransactGetItems.
//
// Every Result is checked (no unchecked deref), so a failure anywhere stops the
// sample with a diagnostic rather than crashing.

#include <cstdio>
#include <string>

#include "ddbsh/connection.hpp"
#include "ddbsh/testing/memory_client.hpp"
#include "ddbsh/value.hpp"

namespace {

// Execute `sql` for its effect and print the byte-exact ddbsh output. Returns
// false (with a diagnostic) on any parse/execution error.
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

// Create the table the sample works against and seed a few rows.
bool seed(ddbsh::Connection& db) {
    return run(db,
               "create table catalog ( id string, year number ) "
               "primary key ( id hash, year range );") &&
           run(db,
               "insert into catalog ( id, year, title ) "
               "values ( \"film\", 2010, \"Inception\" );") &&
           run(db,
               "insert into catalog ( id, year, title ) "
               "values ( \"film\", 2014, \"Interstellar\" );") &&
           run(db,
               "insert into catalog ( id, year, title ) "
               "values ( \"film\", 2017, \"Dunkirk\" );");
}

// ---- 1. Prepared / parameterized statements -------------------------------
//
// prepare() parses the SQL ONCE; each positional `?` becomes a 1-based
// parameter. Bind, execute, then re-bind and re-execute WITHOUT re-parsing.
// Binding is injection-safe: a string bound to `?` can never break out of its
// value position, so building a statement from untrusted input SHOULD bind
// rather than concatenate.
bool prepared_statements(ddbsh::Connection& db) {
    std::puts("\n--- 1. Prepared / parameterized statements ---");

    ddbsh::Result<ddbsh::PreparedStatement> prep =
        db.prepare("select * from catalog where id = ? and year = ?;");
    if (!prep.has_value()) {
        std::fprintf(stderr, "prepare failed: %s\n", prep.error().message.c_str());
        return false;
    }
    std::printf("prepared a statement with %d parameters\n", prep->parameter_count());

    // Execute once.
    prep->bind(1, std::string("film")).bind(2, 2010LL);
    ddbsh::Result<ddbsh::RowSet> rows = db.query(*prep);
    if (!rows.has_value()) {
        std::fprintf(stderr, "exec failed: %s\n", rows.error().message.c_str());
        return false;
    }
    for (const ddbsh::Row& row : *rows) {
        std::printf("  bind year=2010 -> %s\n", row.get_string("title").value_or("?").c_str());
    }

    // RE-execute the SAME prepared statement with a different binding — no
    // re-parse. This is the ODBC SQLExecute / DB-Library dbnextrow loop.
    prep->bind(2, 2014LL);
    rows = db.query(*prep);
    if (!rows.has_value()) {
        std::fprintf(stderr, "re-exec failed: %s\n", rows.error().message.c_str());
        return false;
    }
    for (const ddbsh::Row& row : *rows) {
        std::printf("  bind year=2014 -> %s\n", row.get_string("title").value_or("?").c_str());
    }
    return true;
}

// ---- 2. Typed set / binary bind -------------------------------------------
//
// DynamoDB string-set (SS), number-set (NS), binary-set (BS) and binary (B)
// values have NO ddbsh literal syntax, so the ONLY way to write them is a typed
// bind: Value::string_set / number_set / binary_set / binary_value bound to a
// `?` parameter. The bound Value flows straight through the engine's serializer
// — no textual round-trip, full fidelity.
bool set_and_binary_bind(ddbsh::Connection& db) {
    std::puts("\n--- 2. Typed set / binary bind ---");

    if (!run(db, "create table assets ( id string ) primary key ( id hash );")) {
        return false;
    }

    ddbsh::Result<ddbsh::PreparedStatement> ins =
        db.prepare("insert into assets ( id, labels, scores, blob ) values ( ?, ?, ?, ? );");
    if (!ins.has_value()) {
        std::fprintf(stderr, "prepare failed: %s\n", ins.error().message.c_str());
        return false;
    }
    ins->bind(1, std::string("asset-1"))
        .bind(2, ddbsh::Value::string_set({"red", "green", "blue"}))
        .bind(3, ddbsh::Value::number_set({"1", "2", "3"}))
        .bind(4, ddbsh::Value::binary_value("aGVsbG8="));  // base64("hello")
    if (ddbsh::Result<ddbsh::Results> r = db.exec(*ins); !r.has_value()) {
        std::fprintf(stderr, "insert failed: %s\n", r.error().message.c_str());
        return false;
    }
    std::puts("inserted an item with a string set, a number set, and a binary value");

    // Read the sets/binary back as typed cells.
    ddbsh::Result<ddbsh::RowSet> rows = db.query("select * from assets where id = \"asset-1\";");
    if (!rows.has_value()) {
        std::fprintf(stderr, "read-back failed: %s\n", rows.error().message.c_str());
        return false;
    }
    if (rows->empty()) {
        std::fputs("error: asset-1 not found after insert\n", stderr);
        return false;
    }
    const ddbsh::Row& row = (*rows)[0];
    if (const std::vector<std::string>* labels = row.cell("labels").as_string_set()) {
        std::fputs("  labels (SS):", stdout);
        for (const std::string& t : *labels) {
            std::printf(" %s", t.c_str());
        }
        std::putchar('\n');
    }
    ddbsh::Cell blob = row.cell("blob");
    if (!blob.missing() && blob.type() == ddbsh::Value::Type::B) {
        std::printf("  blob (B): %s (base64)\n", blob.value().s.c_str());
    }
    return true;
}

// ---- 3. Lazy streaming -----------------------------------------------------
//
// A row-returning SELECT pages LAZILY: Results::next_row() pulls the next page
// only when the current one is exhausted, so a large Scan is never fully
// resident. Here we set a deliberately tiny page size on the backend so a
// three-row query crosses page boundaries, and drain it one row at a time.
bool lazy_streaming(ddbsh::Connection& db, ddbsh::testing::MemoryClient& client) {
    std::puts("\n--- 3. Lazy streaming (small page size, row-by-row) ---");

    // Force the backend to return one item per page, so next_row() must fetch
    // across several pages to walk the whole result.
    client.set_page_size(1);

    ddbsh::Result<ddbsh::Results> r = db.exec("select * from catalog where id = \"film\";");
    if (!r.has_value()) {
        std::fprintf(stderr, "stream failed: %s\n", r.error().message.c_str());
        client.set_page_size(0);
        return false;
    }
    ddbsh::Results& res = *r;
    int n = 0;
    while (res.next_row()) {
        std::printf("  page-boundary row %d: year=%lld title=%s\n", ++n,
                    res.column("year").as_int().value_or(-1),
                    res.column("title").as_string().value_or("?").c_str());
    }
    if (auto e = res.error()) {
        std::fprintf(stderr, "stream error: %s\n", e->message.c_str());
        client.set_page_size(0);
        return false;
    }
    std::printf("drained %d rows one page at a time (page size 1)\n", n);

    client.set_page_size(0);  // restore unbounded paging for the rest of the sample
    return true;
}

// ---- 4. Transactions -------------------------------------------------------
//
// A BEGIN … COMMIT body of writes is one atomic transaction (TransactWriteItems
// on the backend): all succeed or none do. A BEGIN … COMMIT body of key-reduced
// SELECTs is a read transaction (TransactGetItems) whose items arrive in one
// rows result set, in request order.
bool transactions(ddbsh::Connection& db) {
    std::puts("\n--- 4. Transactions ---");

    if (!run(db, "create table accounts ( id string ) primary key ( id hash );")) {
        return false;
    }

    // Write transaction: two inserts commit together.
    if (!run(db,
             "begin; "
             "insert into accounts ( id, balance ) values ( \"alice\", 100 ); "
             "insert into accounts ( id, balance ) values ( \"bob\", 50 ); "
             "commit;")) {
        return false;
    }
    std::puts("write transaction committed (alice + bob)");

    // Read transaction: both GetItems arrive in one rows result set.
    ddbsh::Result<ddbsh::Results> r = db.exec(
        "begin; "
        "select * from accounts where id = \"alice\"; "
        "select * from accounts where id = \"bob\"; "
        "commit;");
    if (!r.has_value()) {
        std::fprintf(stderr, "read txn failed: %s\n", r.error().message.c_str());
        return false;
    }
    ddbsh::Results& res = *r;
    while (res.next_row()) {
        std::printf("  read-txn row: id=%s balance=%lld\n",
                    res.column("id").as_string().value_or("?").c_str(),
                    res.column("balance").as_int().value_or(-1));
    }
    if (auto e = res.error()) {
        std::fprintf(stderr, "read txn error: %s\n", e->message.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main() {
    std::puts("=== ddbsh Phase-5 FEATURE embedding sample (in-memory client, no AWS) ===");

    // A genuine in-memory backend and a Connection over it. The MemoryClient is
    // held by reference so the streaming section can adjust its page size.
    ddbsh::testing::MemoryClient client;
    ddbsh::Connection db(ddbsh::SessionConfig{"local", ""}, client);

    if (!seed(db) || !prepared_statements(db) || !set_and_binary_bind(db) ||
        !lazy_streaming(db, client) || !transactions(db)) {
        std::fputs("\n=== feature sample FAILED ===\n", stderr);
        return 1;
    }

    std::puts("\n=== feature sample complete ===");
    return 0;
}
