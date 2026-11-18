// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// harness.mjs — the P6d end-to-end WASM smoke (D-P6d-2).
//
// NOT the product UI (that is P6e's index.html/app.js). This is a throwaway
// Node driver that proves the Asyncify bridge carries a real op round-trip:
// it instantiates the Emscripten ddbsh module, wires Module.submit to the
// dropped-in in-browser ExtendDB adapter, and runs a scripted sequence through
// the DdbshSession embind surface, asserting the rendered output.
//
// Run from the build output directory (where ddbsh-extenddb.js,
// ddbsh-extenddb.wasm, and vendor/extenddb/ are present — ddbsh's own files are
// staged by the web/ POST_BUILD step; the ExtendDB adapter is dropped into
// vendor/extenddb/ by devtools/stage-extenddb-webasm, which
// devtools/run-web-smoke runs for you):
//     node harness.mjs
// Exits 0 on success, nonzero (with a diagnostic) on the first failed
// assertion. The web/ CMake target stages this file beside the module so the
// relative imports below resolve.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { webcrypto } from "node:crypto";

import createDdbsh from "./ddbsh-extenddb.js";
import initExtendDb, { ExtendDb } from "./vendor/extenddb/extenddb_wasm_adapter.js";

// Browser-global shim (D-P6d-2): the dropped-in ExtendDB adapter uses the Web
// Crypto API (`crypto.getRandomValues`, for UUIDs). A browser exposes `crypto`
// natively; Node 18 does not expose it as a global by default, so provide it
// before the adapter runs. This is a Node-harness concern only — it is NOT
// shipped to the browser and is NOT part of the engine. (ESM evaluates all
// imports before this top-level statement, which runs before main() calls
// ExtendDb.create(), so `crypto` is in place when the adapter needs it.)
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

// A single run() call. Tolerates both a synchronous return and a Promise
// (depending on how Asyncify surfaces the embind call — the bridge suspends
// internally, so the outer call may resolve asynchronously).
async function run(session, text) {
    let r = session.run(text);
    if (r && typeof r.then === "function") {
        r = await r;
    }
    return r;
}

async function main() {
    // --- bring up the in-browser ExtendDB engine (in-memory SQLite) ---------
    const adapterWasm = readFileSync(
        fileURLToPath(new URL("./vendor/extenddb/extenddb_wasm_adapter_bg.wasm", import.meta.url)));
    await initExtendDb({ module_or_path: adapterWasm });  // init from bytes (no fetch)
    const db = await ExtendDb.create();

    // --- bring up the ddbsh engine module + wire the submit bridge ----------
    const Module = await createDdbsh();
    Module.submit = (op, body) => db.submit(op, body);

    const session = new Module.DdbshSession("us-east-1");

    console.log("P6d WASM smoke — ddbsh engine over in-browser ExtendDB (Asyncify bridge)");

    // 1) CREATE TABLE (waits for ACTIVE via DescribeTable through the bridge).
    let r = await run(session,
        "create table ddbsh_web_smoke ( p number, s number ) " +
        "primary key ( p hash, s range ) billing mode on demand");
    check(r.ok, "create table ok");
    contains(r.stdout, "CREATE", "create table status token");

    // 2) INSERT three items.
    r = await run(session,
        "insert into ddbsh_web_smoke (p, s, v) values (1, 2, 100), (1, 3, 200), (2, 9, 300)");
    check(r.ok, "insert ok");
    contains(r.stdout, "INSERT", "insert status token");

    // 3) SELECT reducing to GetItem (full composite key).
    r = await run(session, "select * from ddbsh_web_smoke where p = 1 and s = 2");
    check(r.ok, "select GetItem ok");
    contains(r.stdout, "{p: 1, s: 2, v: 100}", "GetItem item render");

    // 4) SELECT reducing to Query (hash only) — both partition items present.
    r = await run(session, "select * from ddbsh_web_smoke where p = 1");
    check(r.ok, "select Query ok");
    contains(r.stdout, "{p: 1, s: 2, v: 100}", "Query item 1 render");
    contains(r.stdout, "{p: 1, s: 3, v: 200}", "Query item 2 render");

    // 5) EXPLAIN SELECT (resolves key schema via DescribeTable through the
    //    bridge) — GetItem and Query planning.
    r = await run(session, "explain select * from ddbsh_web_smoke where p = 1 and s = 2");
    check(r.ok, "explain GetItem ok");
    contains(r.stdout, "GetItem(", "explain GetItem request");
    r = await run(session, "explain select * from ddbsh_web_smoke where p = 1");
    check(r.ok, "explain Query ok");
    contains(r.stdout, "Query(", "explain Query request");

    // 6) UPDATE a single item, then confirm the new value.
    r = await run(session, "update ddbsh_web_smoke set v = 999 where p = 1 and s = 2");
    check(r.ok, "update ok");
    contains(r.stdout, "UPDATE", "update status token");
    r = await run(session, "select * from ddbsh_web_smoke where p = 1 and s = 2");
    contains(r.stdout, "{p: 1, s: 2, v: 999}", "updated value read back");

    // 7) DELETE a single item.
    r = await run(session, "delete from ddbsh_web_smoke where p = 2 and s = 9");
    check(r.ok, "delete ok");
    contains(r.stdout, "DELETE", "delete status token");

    // 8) SHOW LIMITS — the §7 connect/control-plane smoke (DescribeLimits).
    r = await run(session, "show limits");
    check(r.ok, "show limits ok");
    check(r.stdout.length > 0, "show limits produced output");

    // 9) Degradation path (D-P6-7/D-P6-10): a service-level error arrives from
    //    ExtendDB as a `__type` document and is mapped by the codec to the
    //    faithful ddbsh error string through render_error.
    //    (a) INSERT a duplicate key -> ConditionalCheckFailed.
    r = await run(session, "insert into ddbsh_web_smoke (p, s, v) values (1, 2, 1)");
    check(!r.ok, "duplicate insert fails (not ok)");
    contains(r.stderr, "The conditional request failed", "ccf error mapped to ddbsh string");
    //    (b) operate on a non-existent table -> "not found".
    r = await run(session, "select * from ddbsh_web_smoke_absent where p = 1");
    check(!r.ok, "select on absent table fails (not ok)");
    contains(r.stderr, "not found", "absent-table error mapped");

    // 10) DROP TABLE (waits for the table to be gone via DescribeTable).
    r = await run(session, "drop table ddbsh_web_smoke");
    check(r.ok, "drop table ok");
    contains(r.stdout, "DROP", "drop table status token");

    console.log(failures === 0
        ? "\nP6d WASM smoke: PASS"
        : `\nP6d WASM smoke: FAIL (${failures} assertion failure(s))`);
    process.exit(failures === 0 ? 0 : 1);
}

main().catch((e) => {
    console.error("P6d WASM smoke: ERROR", e);
    process.exit(2);
});
