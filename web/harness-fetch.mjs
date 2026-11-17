// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// harness-fetch.mjs — the P6f-a end-to-end WASM smoke for the FETCH edition
// (D-P6f-1, §13 P6f-a: "Node headless harness green for … option 1 against a
// stub").
//
// This is the real-backend `ddbsh-wasm` edition (built with DDBSH_WEB_FETCH, so
// it links ddbsh::wire::FetchTransport, whose Asyncify bridge calls the JS hook
// `Module.fetchDdb(op, body)` — NOT Module.submit). In production that hook
// POSTs to the local ddbsh-web-proxy, which SigV4-signs and forwards to real
// DynamoDB/ExtendDB. P6f-a has no proxy yet (that is P6f-b), so this harness
// STUBS the proxy: Module.fetchDdb is wired to the same in-browser ExtendDB
// adapter the submit harness uses, which returns real DynamoDB-shaped response
// bodies. That proves exactly the P6f-a contract — the FetchTransport bridge
// carries each op's body to the hook and back, Asyncify suspends/resumes across
// the await, and the codec parses the response — without yet needing the native
// proxy or any credentials/network.
//
// Because the fetch edition does NOT link SubmitTransport, the only reason ops
// reach the backend at all is that FetchTransport's bridge calls Module.fetchDdb
// — so a green run is specific to the fetch edition, not an accident of the
// submit path.
//
// Run from the build output directory (where ddbsh-wasm.js, ddbsh-wasm.wasm,
// and vendor/ are staged by the web/ POST_BUILD step):
//     node harness-fetch.mjs
// Exits 0 on success, nonzero (with a diagnostic) on the first failed
// assertion.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { webcrypto } from "node:crypto";

import createDdbsh from "./ddbsh-wasm.js";
import initExtendDb, { ExtendDb } from "./vendor/extenddb/extenddb_wasm_adapter.js";

// Browser-global shim (as in harness.mjs): the vendored ExtendDB adapter uses
// the Web Crypto API; Node 18 does not expose `crypto` as a global by default.
if (!globalThis.crypto) {
    globalThis.crypto = webcrypto;
}

// ---- tiny assertion helpers ------------------------------------------------
let failures = 0;
function check(cond, label) {
    if (cond) {
        console.log(`  ok   ${label}`);
    } else {
        console.error(`  FAIL ${label}`);
        failures += 1;
    }
}
function contains(haystack, needle, label) {
    check(typeof haystack === "string" && haystack.includes(needle),
          `${label} — expected to contain ${JSON.stringify(needle)}; got ${JSON.stringify(haystack)}`);
}

async function run(session, text) {
    let r = session.run(text);
    if (r && typeof r.then === "function") {
        r = await r;
    }
    return r;
}

async function main() {
    // --- bring up the in-browser ExtendDB engine (stands in for the proxy) --
    const adapterWasm = readFileSync(
        fileURLToPath(new URL("./vendor/extenddb/extenddb_wasm_adapter_bg.wasm", import.meta.url)));
    await initExtendDb({ module_or_path: adapterWasm });
    const db = await ExtendDb.create();

    // --- bring up the FETCH-edition ddbsh module + wire the fetch bridge ----
    const Module = await createDdbsh();
    // The whole point: the fetch edition drives Module.fetchDdb, so wiring it
    // (and NOT Module.submit) is what makes ops flow. In production this hook is
    // `fetch('/ddb', …)` to the proxy; here it is the ExtendDB stub.
    let fetchCalls = 0;
    Module.fetchDdb = (op, body) => {
        fetchCalls += 1;
        return db.submit(op, body);
    };

    const session = new Module.DdbshSession("us-east-1");

    console.log("P6f-a WASM smoke — ddbsh FETCH edition over a stubbed proxy (Asyncify bridge)");

    // 0) SHOW CONNECTION — proves the descriptor seam end-to-end (D-P6f-9):
    //    the fetch edition reports the FetchTransport binding. No backend call.
    let r = await run(session, "show connection");
    check(r.ok, "show connection ok");
    contains(r.stdout, "ddbsh - version ", "show connection reports the library version");
    contains(r.stdout, "wire/fetch -> POST /ddb (proxy)",
             "show connection reports the FetchTransport backend binding");
    contains(r.stdout, "Region: us-east-1", "show connection reports the region");

    // 1) CREATE TABLE (waits for ACTIVE via DescribeTable through the bridge).
    r = await run(session,
        "create table ddbsh_web_fetch_smoke ( p number, s number ) " +
        "primary key ( p hash, s range ) billing mode on demand");
    check(r.ok, "create table ok");
    contains(r.stdout, "CREATE", "create table status token");

    // 2) INSERT three items.
    r = await run(session,
        "insert into ddbsh_web_fetch_smoke (p, s, v) values (1, 2, 100), (1, 3, 200), (2, 9, 300)");
    check(r.ok, "insert ok");
    contains(r.stdout, "INSERT", "insert status token");

    // 3) SELECT reducing to GetItem (full composite key).
    r = await run(session, "select * from ddbsh_web_fetch_smoke where p = 1 and s = 2");
    check(r.ok, "select GetItem ok");
    contains(r.stdout, "{p: 1, s: 2, v: 100}", "GetItem item render");

    // 4) SELECT reducing to Query (hash only) — both partition items present.
    r = await run(session, "select * from ddbsh_web_fetch_smoke where p = 1");
    check(r.ok, "select Query ok");
    contains(r.stdout, "{p: 1, s: 2, v: 100}", "Query item 1 render");
    contains(r.stdout, "{p: 1, s: 3, v: 200}", "Query item 2 render");

    // 5) EXPLAIN SELECT (resolves key schema via DescribeTable through the
    //    bridge) — GetItem and Query planning.
    r = await run(session, "explain select * from ddbsh_web_fetch_smoke where p = 1 and s = 2");
    check(r.ok, "explain GetItem ok");
    contains(r.stdout, "GetItem(", "explain GetItem request");
    r = await run(session, "explain select * from ddbsh_web_fetch_smoke where p = 1");
    check(r.ok, "explain Query ok");
    contains(r.stdout, "Query(", "explain Query request");

    // 6) UPDATE a single item, then confirm the new value.
    r = await run(session, "update ddbsh_web_fetch_smoke set v = 999 where p = 1 and s = 2");
    check(r.ok, "update ok");
    contains(r.stdout, "UPDATE", "update status token");
    r = await run(session, "select * from ddbsh_web_fetch_smoke where p = 1 and s = 2");
    contains(r.stdout, "{p: 1, s: 2, v: 999}", "updated value read back");

    // 7) DELETE a single item.
    r = await run(session, "delete from ddbsh_web_fetch_smoke where p = 2 and s = 9");
    check(r.ok, "delete ok");
    contains(r.stdout, "DELETE", "delete status token");

    // 8) Degradation path: a service-level error arrives from the backend as a
    //    `__type` document and is mapped by the codec to the faithful ddbsh
    //    error string — proving the fetch path handles errors identically.
    r = await run(session, "insert into ddbsh_web_fetch_smoke (p, s, v) values (1, 2, 1)");
    check(!r.ok, "duplicate insert fails (not ok)");
    contains(r.stderr, "The conditional request failed", "ccf error mapped to ddbsh string");

    // 9) DROP TABLE (waits for the table to be gone via DescribeTable).
    r = await run(session, "drop table ddbsh_web_fetch_smoke");
    check(r.ok, "drop table ok");
    contains(r.stdout, "DROP", "drop table status token");

    // The fetch bridge must have actually carried ops (not a no-op pass).
    check(fetchCalls > 0, `Module.fetchDdb was exercised (${fetchCalls} calls)`);

    console.log(failures === 0
        ? "\nP6f-a WASM fetch smoke: PASS"
        : `\nP6f-a WASM fetch smoke: FAIL (${failures} assertion failure(s))`);
    process.exit(failures === 0 ? 0 : 1);
}

main().catch((e) => {
    console.error("P6f-a WASM fetch smoke: ERROR", e);
    process.exit(2);
});
