// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// harness-extenddb.mjs — the P6f-c end-to-end web smoke against a REAL local
// ExtendDB server through the native ddbsh-web-proxy (§13 P6f-c: "Node harness
// green against local ExtendDB").
//
// A thin wrapper over the shared corpus in harness-proxy-lib.mjs, identical to
// harness-proxy.mjs except for the backend wiring that the proxy's `/connect`
// carries. Where harness-proxy signs for real AWS DynamoDB with the default
// credential chain, this harness points the fetch edition at a local ExtendDB
// server (HTTPS, self-signed cert) by driving:
//
//     connect <region> with endpoint "<extenddb-endpoint>"
//
// and supplying the proxy the PER-ENDPOINT credential wiring ExtendDB needs:
//   * a named AWS profile holding the key ExtendDB issued (DDBSH_EXTENDDB_PROFILE),
//   * a CA bundle to trust ExtendDB's self-signed cert (DDBSH_EXTENDDB_CA_BUNDLE).
//
// Neither is a secret — a profile NAME and a CA file PATH. The actual key stays
// in the AWS credentials file the proxy reads; no credential crosses this
// harness or the browser seam (D-P6f-7). ExtendDB is HTTPS-only — plain HTTP is
// never used.
//
// This exercises the proxy's per-endpoint `profile` + `ca_bundle` mechanism
// (landed in P6f-b) end-to-end against a real ExtendDB for the first time.
//
// Env (all set by devtools/run-extenddb-smoke):
//   DDBSH_PROXY_URL         default http://127.0.0.1:8460
//   DDBSH_REGION            default us-east-1
//   DDBSH_EXTENDDB_ENDPOINT default https://127.0.0.1:18443
//   DDBSH_EXTENDDB_PROFILE  required — the AWS profile the proxy resolves
//   DDBSH_EXTENDDB_CA_BUNDLE required — the self-signed cert to trust
// Run from the wasm build output dir:  node harness-extenddb.mjs
// Exits 0 on success, nonzero on the first failed assertion.

import { randomBytes } from "node:crypto";

import createDdbsh from "./ddbsh-wasm.js";
import { runCorpus } from "./harness-proxy-lib.mjs";

const PROXY_URL = process.env.DDBSH_PROXY_URL || "http://127.0.0.1:8460";
const REGION = process.env.DDBSH_REGION || "us-east-1";
const ENDPOINT = process.env.DDBSH_EXTENDDB_ENDPOINT || "https://127.0.0.1:18443";
const PROFILE = process.env.DDBSH_EXTENDDB_PROFILE;
const CA_BUNDLE = process.env.DDBSH_EXTENDDB_CA_BUNDLE;
const TABLE = `ddbsh_web_extenddb_smoke_${randomBytes(4).toString("hex")}`;

async function main() {
    if (!PROFILE || !CA_BUNDLE) {
        // A missing credential/trust input is an INFRASTRUCTURE failure, not a
        // skip (shared steering: no test is skipped).
        console.error(
            "harness-extenddb: DDBSH_EXTENDDB_PROFILE and DDBSH_EXTENDDB_CA_BUNDLE " +
            "are required (set by devtools/run-extenddb-smoke).");
        process.exit(2);
    }

    const { pass, failures } = await runCorpus({
        moduleFactory: createDdbsh,
        proxyUrl: PROXY_URL,
        region: REGION,
        endpoint: ENDPOINT,
        // ExtendDB: the proxy resolves the ExtendDB-issued key from this named
        // profile and trusts the self-signed cert via this CA bundle.
        connectExtra: { profile: PROFILE, ca_bundle: CA_BUNDLE },
        table: TABLE,
        label: "P6f-c WASM smoke — ddbsh FETCH edition over the REAL ddbsh-web-proxy (local ExtendDB, HTTPS)",
    });

    console.log(pass
        ? "\nP6f-c WASM ExtendDB smoke: PASS"
        : `\nP6f-c WASM ExtendDB smoke: FAIL (${failures} assertion failure(s))`);
    process.exit(pass ? 0 : 1);
}

main().catch((e) => {
    console.error("P6f-c WASM ExtendDB smoke: ERROR", e);
    process.exit(2);
});
