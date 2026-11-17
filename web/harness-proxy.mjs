// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// harness-proxy.mjs — the P6f-b end-to-end web smoke against REAL DynamoDB
// through the native ddbsh-web-proxy (D-P6f-3, §13 P6f-b: "Node harness green
// against real DynamoDB").
//
// A thin wrapper over the shared corpus in harness-proxy-lib.mjs. It loads the
// fetch edition (`ddbsh-wasm`, FetchTransport), performs the browser's /session
// + /connect handshake against a RUNNING ddbsh-web-proxy, wires
// `Module.fetchDdb`/`Module.connectDdb` to POST to it, and runs a full
// CREATE -> INSERT -> SELECT(GetItem/Query) -> EXPLAIN -> UPDATE -> DELETE ->
// ccf -> DROP corpus against real DynamoDB. The proxy SigV4-signs every /ddb
// forward with credentials resolved from the host's AWS chain; no credential
// ever reaches this harness (it only holds the per-session token). For AWS there
// is no endpoint override and no per-endpoint credential wiring (the default
// chain signs) — contrast harness-extenddb.mjs (P6f-c).
//
// WARNING: this talks to a REAL AWS account. It creates and drops a single
// `ddbsh_web_proxy_*`-prefixed on-demand table and cleans it up (even on
// failure). Run only against a dedicated test account/region.
//
// Env: DDBSH_PROXY_URL (default http://127.0.0.1:8459), DDBSH_REGION
// (default us-east-1). Run from the wasm build output dir (where ddbsh-wasm.js
// is staged):  node harness-proxy.mjs
// Exits 0 on success, nonzero on the first failed assertion.

import { randomBytes } from "node:crypto";

import createDdbsh from "./ddbsh-wasm.js";
import { runCorpus } from "./harness-proxy-lib.mjs";

const PROXY_URL = process.env.DDBSH_PROXY_URL || "http://127.0.0.1:8459";
const REGION = process.env.DDBSH_REGION || "us-east-1";
const TABLE = `ddbsh_web_proxy_smoke_${randomBytes(4).toString("hex")}`;

async function main() {
    const { pass, failures } = await runCorpus({
        moduleFactory: createDdbsh,
        proxyUrl: PROXY_URL,
        region: REGION,
        // AWS: no endpoint override, no per-endpoint credential wiring — the
        // proxy signs with the host's default AWS credential chain.
        table: TABLE,
        label: "P6f-b WASM smoke — ddbsh FETCH edition over the REAL ddbsh-web-proxy (real DynamoDB)",
    });

    console.log(pass
        ? "\nP6f-b WASM proxy smoke: PASS"
        : `\nP6f-b WASM proxy smoke: FAIL (${failures} assertion failure(s))`);
    process.exit(pass ? 0 : 1);
}

main().catch((e) => {
    console.error("P6f-b WASM proxy smoke: ERROR", e);
    process.exit(2);
});
