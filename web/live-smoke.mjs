// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// live-smoke.mjs — the browser edition's live UI-path smoke (P9 §4.5).
//
// Drives DdbshSession.run (the path the page's editor uses, run_batch) through
// the same bridge, vendored SDK and credential validation as the page, against
// REAL DynamoDB: create a `ddbsh_web_smoke_<pid>` table and wait for ACTIVE,
// insert, a Query-selecting SELECT, UPDATE, a write transaction, EXPLAIN, SHOW
// CONNECTION, a rejected CONNECT, and DROP (always, in `finally`). The full
// golden corpus is devtools/run-web-golden's job; this proves the UI entry point
// and its ordered segments.
//
// Env: DDBSH_WEB_CREDS_FILE (minted by devtools/run-web-smoke; the `default`
// entry is used), AWS_REGION (default us-east-1). Needs Node >= 20.
//
// WARNING: creates and drops one real table (ddbsh_web_smoke_<pid>).

import { readFileSync } from "node:fs";

import * as sdk from "./vendor/aws-sdk/aws-sdk-dynamodb.mjs";
import { ALLOWED_HOSTS, REGIONS, SDK_VERSION } from "./vendor/aws-sdk/allowed-hosts.mjs";
import { buildTranscriptEntries, parseCredentials } from "./app-core.mjs";
import { createSdkBridge, installHooks } from "./sdk-bridge.mjs";
import createDdbsh from "./ddbsh.js";

let failures = 0;
function check(cond, label, detail = "") {
    if (cond) {
        console.log(`  ok   ${label}`);
    } else {
        console.error(`  FAIL ${label}${detail ? `\n${detail}` : ""}`);
        failures += 1;
    }
}

const region = process.env.AWS_REGION || "us-east-1";
const entry = JSON.parse(readFileSync(process.env.DDBSH_WEB_CREDS_FILE, "utf8")).default;
const parsed = parseCredentials(JSON.stringify({ Credentials: entry }));
if (!parsed.ok) {
    console.error(`live-smoke: ${parsed.message}`);
    process.exit(1);
}
const bridge = createSdkBridge({ sdk, allowedHosts: ALLOWED_HOSTS, regions: REGIONS,
    sdkVersion: SDK_VERSION, region });
check(bridge.setCredentials(parsed.credentials).ok, "minted credentials accepted by the page's validation");

const Module = await createDdbsh();
installHooks(Module, bridge);
const session = new Module.DdbshSession(region);

async function run(text) {
    let r = session.run(text);
    if (r && typeof r.then === "function") {
        r = await r;
    }
    return r;
}
const show = (r) => `--- stdout\n${r.stdout}--- stderr\n${r.stderr}`;

const table = `ddbsh_web_smoke_${process.pid}`;
try {
    let r = await run(`create table ${table} ( pk number, rk number ) primary key ( pk hash, rk range );`);
    check(r.ok && r.stdout === "CREATE\n", "CREATE TABLE (waits for ACTIVE)", show(r));

    r = await run(`insert into ${table} (pk, rk, v) values (1, 1, "a"), (1, 2, "b"), (2, 1, "c");`);
    check(r.ok && r.stdout === "INSERT\nINSERT\nINSERT\n", "multi-row INSERT", show(r));

    r = await run(`select * from ${table} where pk = 1;`);
    check(r.ok && r.stdout === "{pk: 1, rk: 1, v: a}\n{pk: 1, rk: 2, v: b}\n", "Query-selecting SELECT", show(r));

    r = await run(`explain select * from ${table} where pk = 1;`);
    check(r.ok && r.stdout.startsWith("Query({") && r.stdout.includes(`"TableName":   "${table}"`),
        "EXPLAIN SELECT renders the Query request", show(r));

    r = await run(`update ${table} set v = "z" where pk = 1;`);
    check(r.ok && r.stdout === "UPDATE (2 read, 2 modified, 0 ccf)\n", "bulk UPDATE", show(r));

    r = await run(`begin; insert into ${table} (pk, rk) values (3, 1); delete from ${table} where pk = 2 and rk = 1; commit;`);
    check(r.ok && r.stdout === "COMMIT\n", "write transaction", show(r));

    r = await run(`select * from ${table};`);
    const rows = r.stdout.split("\n").filter(Boolean).sort();
    check(r.ok && JSON.stringify(rows) === JSON.stringify(
        ["{pk: 1, rk: 1, v: z}", "{pk: 1, rk: 2, v: z}", "{pk: 3, rk: 1}"]), "Scan after the writes", show(r));

    r = await run(`select * from ${table} where pk = 1; select * from no_such_table_${process.pid}; show connection;`);
    const kinds = buildTranscriptEntries("p> ", "", r).map((e) => e.kind);
    check(JSON.stringify(kinds) === JSON.stringify(["stdout", "stderr", "stdout"]),
        "segments keep the CLI's stdout/stderr interleaving", JSON.stringify(r.segments));
    check(r.stdout.includes(`Backend: wire/aws-sdk-js ${SDK_VERSION} -> https://dynamodb.${region}.amazonaws.com (default); credentials ASIA`),
        "SHOW CONNECTION names the browser backend", show(r));
    check(!r.stdout.includes(entry.SecretAccessKey) && !r.stdout.includes(entry.SessionToken),
        "SHOW CONNECTION reveals no secret or token");

    r = await run(`connect us-east-1 with endpoint "https://evil.example.com"; select * from ${table} where pk = 1; connect ${region};`);
    check(r.stdout.startsWith("CONNECT\n") && r.stderr.includes("browser: CONNECT rejected: endpoint https://evil.example.com is not permitted") &&
        r.stderr.includes("wire transport: CONNECT rejected"), "rejected CONNECT fails closed and is reported", show(r));
    r = await run(`select * from ${table} where pk = 3;`);
    check(r.ok && r.stdout === "{pk: 3, rk: 1}\n", "a valid CONNECT restores service", show(r));
} finally {
    const r = await run(`drop table if exists ${table};`);
    check(r.ok, `DROP TABLE ${table} (cleanup)`, show(r));
}

if (failures > 0) {
    console.error(`\nlive smoke: ${failures} FAILED`);
    process.exit(1);
}
console.log("\nlive smoke: all passed");
process.exit(0);
