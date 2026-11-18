// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// harness-proxy-lib.mjs — the shared browser-emulating handshake + data-plane
// corpus driven by BOTH real-backend proxy smokes (phase 6f):
//
//   * harness-proxy.mjs    — the fetch edition over the real ddbsh-web-proxy
//                            against REAL Amazon DynamoDB (P6f-b, run-proxy-smoke).
//   * harness-extenddb.mjs — the fetch edition over the real ddbsh-web-proxy
//                            against a REAL local ExtendDB server (P6f-c,
//                            run-extenddb-smoke).
//
// Both editions share ONE engine, ONE ddbsh-wire codec, ONE FetchTransport, and
// ONE corpus; they differ only in the CONNECT endpoint and the per-endpoint
// credential wiring the proxy's `/connect` carries (none for AWS — the default
// chain; a `profile` + `ca_bundle` for ExtendDB). Keeping the corpus here, with
// the two harnesses as thin wrappers, mirrors the project's "one engine, two
// transports" design and avoids a near-duplicate second corpus.
//
// The harness emulates the browser faithfully with node:http directly (not the
// global fetch) so it can set the `Origin` header the browser sets automatically
// — the proxy's driver guard requires it. The `Host` header node:http derives
// from the target authority (127.0.0.1:PORT) already matches the guard.
//
// Credentials never reach this harness: it holds only the per-session token. The
// proxy resolves credentials (the AWS chain, or a named profile for ExtendDB)
// and SigV4-signs every /ddb forward.

import http from "node:http";

// ---- tiny assertion helpers ------------------------------------------------

/// A per-run assertion recorder. Each harness constructs one, runs the corpus,
/// then reads `.failures`.
export class Checks {
    constructor() {
        this.failures = 0;
    }
    check(cond, label) {
        if (cond) {
            console.log(`  ok   ${label}`);
        } else {
            console.error(`  FAIL ${label}`);
            this.failures += 1;
        }
    }
    contains(haystack, needle, label) {
        this.check(
            typeof haystack === "string" && haystack.includes(needle),
            `${label} — expected to contain ${JSON.stringify(needle)}; got ${JSON.stringify(haystack)}`,
        );
    }
}

// ---- node:http request to the proxy (full header control) ------------------

/// POST/GET to the proxy with explicit Origin (the browser sets it; the proxy's
/// driver guard requires it). `proxyUrl` is a parsed URL.
function proxyRequest(proxyUrl, path, { method = "POST", headers = {}, body = null } = {}) {
    const origin = `http://${proxyUrl.host}`; // host includes the port
    return new Promise((resolve, reject) => {
        const req = http.request(
            {
                hostname: proxyUrl.hostname,
                port: proxyUrl.port,
                path,
                method,
                headers: { origin, ...headers },
            },
            (res) => {
                let data = "";
                res.setEncoding("utf8");
                res.on("data", (c) => (data += c));
                res.on("end", () => resolve({ status: res.statusCode, text: data }));
            },
        );
        req.on("error", reject);
        if (body != null) {
            req.write(body);
        }
        req.end();
    });
}

async function run(session, text) {
    let r = session.run(text);
    if (r && typeof r.then === "function") {
        r = await r;
    }
    return r;
}

/// Mint a proxy session + token (the browser handshake at page load). Also
/// returns the proxy version the handshake reports (surfaced by SHOW CONNECTION
/// via Module.ddbshProxyVersion, D-P6f-9).
async function mintSession(proxyUrl) {
    const sess = await proxyRequest(proxyUrl, "/session", {
        headers: { "content-type": "application/json" },
    });
    if (sess.status !== 200) {
        throw new Error(`/session failed: HTTP ${sess.status}: ${sess.text}`);
    }
    const { session: sessionId, token, version } = JSON.parse(sess.text);
    if (!sessionId || !token) {
        throw new Error(`/session returned no session/token: ${sess.text}`);
    }
    return { sessionId, token, version: version || "" };
}

/// Wire `Module.connectDdb` / `Module.fetchDdb` on the loaded wasm module so the
/// engine's CONNECT + data-plane calls POST to the running proxy. `connectExtra`
/// is merged into the /connect body (the per-endpoint `profile` + `ca_bundle`
/// for ExtendDB; empty for AWS). Returns a `{ fetchCalls() }` counter.
function wireBridges(Module, proxyUrl, authHeaders, connectExtra) {
    // CONNECT control request: FetchTransport::on_connect fires this. The engine
    // passes (region, endpoint); the glue adds the per-endpoint credential
    // wiring. Empty string returned = success.
    Module.connectDdb = async (region, endpoint) => {
        const payload = { region, ...connectExtra };
        if (endpoint) {
            payload.endpoint = endpoint;
        }
        const r = await proxyRequest(proxyUrl, "/connect", {
            headers: { "content-type": "application/json", ...authHeaders },
            body: JSON.stringify(payload),
        });
        if (r.status === 200) {
            return "";
        }
        try {
            const j = JSON.parse(r.text);
            return j.message || `connect HTTP ${r.status}`;
        } catch {
            return `connect HTTP ${r.status}`;
        }
    };

    // Data-plane forward. A 2xx returns the body verbatim; a service error
    // (4xx/5xx carrying a `__type` document) is returned to the codec to map to
    // the faithful ddbsh error string; a proxy/transport error throws.
    let fetchCalls = 0;
    Module.fetchDdb = async (op, body) => {
        fetchCalls += 1;
        const r = await proxyRequest(proxyUrl, "/ddb", {
            headers: {
                "content-type": "application/x-amz-json-1.0",
                "x-ddbsh-op": op,
                ...authHeaders,
            },
            body,
        });
        if (r.status >= 200 && r.status < 300) {
            return r.text;
        }
        try {
            const j = JSON.parse(r.text);
            if (j && j.__type) {
                return r.text; // service error -> codec maps it
            }
        } catch {
            // fall through to a transport-level failure
        }
        throw new Error(`proxy /ddb HTTP ${r.status}: ${r.text.slice(0, 200)}`);
    };

    return { fetchCalls: () => fetchCalls };
}

/// Run the full end-to-end corpus against a running proxy.
///
/// Options:
///   moduleFactory — the ddbsh-wasm ES-module default export (createDdbsh).
///   proxyUrl      — proxy base URL string (e.g. "http://127.0.0.1:8459").
///   region        — signing region (e.g. "us-east-1").
///   endpoint      — optional endpoint override; present for ExtendDB
///                   ("https://127.0.0.1:18443"), absent for AWS.
///   connectExtra  — extra /connect body fields (ExtendDB: {profile, ca_bundle}).
///   table         — the table name to create/drop.
///   label         — a human label for the log banner.
///   showTablesBurst — optional count N (default 0). When > 0, after the main
///                   corpus the harness creates N extra `${table}_burst_*`
///                   tables (NOWAIT) to provoke the SHOW TABLES
///                   DescribeTable/DescribeTimeToLive burst (2N control calls),
///                   runs SHOW TABLES, and asserts EVERY burst table renders a
///                   FULL status row (a `"<name> | "` line), i.e. zero name-only
///                   fallback rows. This is the HT-1 §6.2 happy-path-over-a-burst
///                   check. HONEST CAVEAT: throttling is probabilistic — a green
///                   run proves the burst still renders fully, NOT that the retry
///                   fired (the deterministic retry proof is the native
///                   wire_client_retry_test suite). The burst tables are dropped
///                   (NOWAIT, best-effort) in teardown.
///
/// Returns { pass, failures, fetchCalls }. Always attempts table teardown.
export async function runCorpus({
    moduleFactory,
    proxyUrl,
    region,
    endpoint = null,
    connectExtra = {},
    table,
    label,
    showTablesBurst = 0,
}) {
    const url = new URL(proxyUrl);
    const checks = new Checks();
    const burstTables = [];

    // --- browser handshake: mint a session + token ------------------------
    const { sessionId, token, version: proxyVersion } = await mintSession(url);
    const authHeaders = { "x-ddbsh-session": sessionId, "x-ddbsh-token": token };

    // --- bring up the FETCH-edition ddbsh module + wire the bridges -------
    const Module = await moduleFactory();
    // Record the proxy version so the engine's SHOW CONNECTION descriptor can
    // surface it (D-P6f-9); the glue sets this exactly as the browser UI does.
    if (proxyVersion) {
        Module.ddbshProxyVersion = proxyVersion;
    }
    const counter = wireBridges(Module, url, authHeaders, connectExtra);

    const session = new Module.DdbshSession(region);

    console.log(label);
    console.log(`  proxy:    http://${url.host}`);
    console.log(`  region:   ${region}`);
    if (endpoint) {
        console.log(`  endpoint: ${endpoint}`);
    }
    console.log(`  table:    ${table}`);

    // The CONNECT statement: with an explicit endpoint for ExtendDB, bare for
    // AWS. The endpoint string must be double-quoted (grammar: a `string`).
    const connectStmt = endpoint
        ? `connect ${region} with endpoint "${endpoint}"`
        : `connect ${region}`;

    let ok = true;
    try {
        // 0) SHOW CONNECTION — descriptor seam, no backend call. Before CONNECT
        //    the descriptor names the FetchTransport binding and (D-P6f-9) the
        //    live proxy version recorded from the /session handshake.
        let r = await run(session, "show connection");
        checks.check(r.ok, "show connection ok");
        checks.contains(r.stdout, "wire/fetch",
            "show connection reports the FetchTransport backend binding");
        if (proxyVersion) {
            checks.contains(r.stdout, proxyVersion,
                "show connection reports the live proxy version");
        }

        // 1) CONNECT — drives FetchTransport::on_connect -> Module.connectDdb ->
        //    the proxy's /connect, establishing this session's signing target.
        r = await run(session, connectStmt);
        checks.check(r.ok, "connect ok");
        checks.contains(r.stdout, "CONNECT", "connect status token");

        // 1b) SHOW CONNECTION after CONNECT — with an explicit endpoint override
        //     the descriptor is enriched with the connected endpoint (D-P6f-9).
        if (endpoint) {
            r = await run(session, "show connection");
            checks.check(r.ok, "show connection (post-connect) ok");
            checks.contains(r.stdout, endpoint,
                "show connection reports the connected endpoint after CONNECT");
        }

        // 2) CREATE TABLE (waits for ACTIVE via DescribeTable through the proxy).
        r = await run(session,
            `create table ${table} ( p number, s number ) ` +
            "primary key ( p hash, s range ) billing mode on demand");
        checks.check(r.ok, "create table ok");
        checks.contains(r.stdout, "CREATE", "create table status token");

        // 3) INSERT three items.
        r = await run(session,
            `insert into ${table} (p, s, v) values (1, 2, 100), (1, 3, 200), (2, 9, 300)`);
        checks.check(r.ok, "insert ok");
        checks.contains(r.stdout, "INSERT", "insert status token");

        // 4) SELECT reducing to GetItem (full composite key).
        r = await run(session, `select * from ${table} where p = 1 and s = 2`);
        checks.check(r.ok, "select GetItem ok");
        checks.contains(r.stdout, "{p: 1, s: 2, v: 100}", "GetItem item render");

        // 5) SELECT reducing to Query (hash only).
        r = await run(session, `select * from ${table} where p = 1`);
        checks.check(r.ok, "select Query ok");
        checks.contains(r.stdout, "{p: 1, s: 2, v: 100}", "Query item 1 render");
        checks.contains(r.stdout, "{p: 1, s: 3, v: 200}", "Query item 2 render");

        // 6) EXPLAIN (resolves key schema via DescribeTable through the proxy).
        r = await run(session, `explain select * from ${table} where p = 1 and s = 2`);
        checks.check(r.ok, "explain GetItem ok");
        checks.contains(r.stdout, "GetItem(", "explain GetItem request");
        r = await run(session, `explain select * from ${table} where p = 1`);
        checks.check(r.ok, "explain Query ok");
        checks.contains(r.stdout, "Query(", "explain Query request");

        // 7) UPDATE a single item, then confirm the new value.
        r = await run(session, `update ${table} set v = 999 where p = 1 and s = 2`);
        checks.check(r.ok, "update ok");
        checks.contains(r.stdout, "UPDATE", "update status token");
        r = await run(session, `select * from ${table} where p = 1 and s = 2`);
        checks.contains(r.stdout, "{p: 1, s: 2, v: 999}", "updated value read back");

        // 8) DELETE a single item.
        r = await run(session, `delete from ${table} where p = 2 and s = 9`);
        checks.check(r.ok, "delete ok");
        checks.contains(r.stdout, "DELETE", "delete status token");

        // 9) ccf: a conditional INSERT over an existing key fails; the backend's
        //    ConditionalCheckFailed `__type` document is mapped by the codec.
        r = await run(session, `insert into ${table} (p, s, v) values (1, 2, 1)`);
        checks.check(!r.ok, "duplicate insert fails (not ok)");
        checks.contains(r.stderr, "The conditional request failed",
            "ccf error mapped to ddbsh string");

        // 10) HT-1 §6.2 — SHOW TABLES over a describe BURST. Create N extra
        //     tables (NOWAIT, fast) so SHOW TABLES issues a burst of 2*(N+…)
        //     DescribeTable/DescribeTimeToLive calls that can trip the account's
        //     control-plane throttle. Assert EVERY burst table renders a FULL
        //     status row (a `"<name> | "` line), i.e. the WireClient retry (not a
        //     name-only fallback) absorbed any throttle. CAVEAT: a green run does
        //     not prove the retry fired (throttling is probabilistic); the
        //     deterministic proof is the native retry suite.
        if (showTablesBurst > 0) {
            for (let i = 0; i < showTablesBurst; i += 1) {
                const bt = `${table}_burst_${i}`;
                burstTables.push(bt);
                const cr = await run(session,
                    `create table ${bt} ( p number ) primary key ( p hash ) ` +
                    "billing mode on demand nowait");
                checks.check(cr.ok, `burst create ${bt} ok`);
            }
            r = await run(session, "show tables");
            checks.check(r.ok, "show tables (burst) ok");
            for (const bt of burstTables) {
                checks.contains(r.stdout, `${bt} | `,
                    `burst table ${bt} renders a FULL status row (no name-only fallback)`);
            }
        }
    } catch (e) {
        console.error(`${label}: ERROR during corpus`, e);
        ok = false;
    } finally {
        // Teardown: drop the table (best-effort, even on failure) so no table is
        // leaked.
        try {
            const r = await run(session, `drop table if exists ${table} nowait`);
            console.log(`  (teardown) drop ${table}: ${r.ok ? "ok" : "failed"}`);
        } catch (e) {
            console.error(`  (teardown) drop ${table} ERROR`, e);
        }
        // Teardown the HT-1 burst tables too (best-effort, NOWAIT).
        for (const bt of burstTables) {
            try {
                const r = await run(session, `drop table if exists ${bt} nowait`);
                console.log(`  (teardown) drop ${bt}: ${r.ok ? "ok" : "failed"}`);
            } catch (e) {
                console.error(`  (teardown) drop ${bt} ERROR`, e);
            }
        }
    }

    const fetchCalls = counter.fetchCalls();
    checks.check(fetchCalls > 0, `Module.fetchDdb was exercised (${fetchCalls} calls)`);

    const pass = ok && checks.failures === 0;
    return { pass, failures: checks.failures, fetchCalls };
}
