// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// harness-proxy.mjs — the P6f-b end-to-end web smoke against REAL DynamoDB
// through the native ddbsh-web-proxy (D-P6f-3, §13 P6f-b: "Node harness green
// against real DynamoDB").
//
// Unlike harness-fetch.mjs (which stubs the proxy with in-tab ExtendDB), this
// harness drives the real relay: it loads the fetch edition (`ddbsh-wasm`,
// FetchTransport), performs the browser's /session + /connect handshake against
// a RUNNING ddbsh-web-proxy, wires `Module.fetchDdb`/`Module.connectDdb` to POST
// to it, and runs a full CREATE -> INSERT -> SELECT(GetItem/Query) -> EXPLAIN ->
// UPDATE -> DELETE -> ccf -> DROP corpus against real DynamoDB. The proxy SigV4-
// signs every /ddb forward with credentials resolved from the host's AWS chain;
// no credential ever reaches this harness (it only holds the per-session token).
//
// It emulates the browser faithfully by using node:http directly (not global
// fetch) so it can set the `Origin` header the browser sets automatically — the
// proxy's driver guard requires it. The `Host` header node:http sets from the
// target authority (127.0.0.1:PORT) already matches the guard.
//
// WARNING: this talks to a REAL AWS account. It creates and drops a single
// `ddbsh_web_proxy_*`-prefixed on-demand table and cleans it up (even on
// failure). Run only against a dedicated test account/region.
//
// Env: DDBSH_PROXY_URL (default http://127.0.0.1:8459), DDBSH_REGION
// (default us-east-1). Run from the wasm build output dir (where ddbsh-wasm.js
// is staged):  node harness-proxy.mjs
// Exits 0 on success, nonzero on the first failed assertion.

import http from "node:http";
import { randomBytes } from "node:crypto";

import createDdbsh from "./ddbsh-wasm.js";

const PROXY_URL = process.env.DDBSH_PROXY_URL || "http://127.0.0.1:8459";
const REGION = process.env.DDBSH_REGION || "us-east-1";
const PROXY = new URL(PROXY_URL);
const PROXY_ORIGIN = `http://${PROXY.host}`; // host includes the port

const TABLE = `ddbsh_web_proxy_smoke_${randomBytes(4).toString("hex")}`;

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

// ---- node:http request to the proxy (full header control) ------------------
function proxyRequest(path, { method = "POST", headers = {}, body = null } = {}) {
    return new Promise((resolve, reject) => {
        const req = http.request(
            {
                hostname: PROXY.hostname,
                port: PROXY.port,
                path,
                method,
                // The browser sets Origin automatically to the page's origin; we
                // set it explicitly to the proxy's own origin (the served page).
                headers: { origin: PROXY_ORIGIN, ...headers },
            },
            (res) => {
                let data = "";
                res.setEncoding("utf8");
                res.on("data", (c) => (data += c));
                res.on("end", () => resolve({ status: res.statusCode, text: data }));
            });
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

async function main() {
    // --- browser handshake: mint a session + token -------------------------
    const sess = await proxyRequest("/session", { headers: { "content-type": "application/json" } });
    if (sess.status !== 200) {
        throw new Error(`/session failed: HTTP ${sess.status}: ${sess.text}`);
    }
    const { session: sessionId, token } = JSON.parse(sess.text);
    if (!sessionId || !token) {
        throw new Error(`/session returned no session/token: ${sess.text}`);
    }
    const authHeaders = { "x-ddbsh-session": sessionId, "x-ddbsh-token": token };

    // --- bring up the FETCH-edition ddbsh module + wire the bridges ---------
    const Module = await createDdbsh();

    // CONNECT control request: the engine's `connect` statement fires this via
    // the FetchTransport::on_connect Asyncify bridge. Empty string = success.
    Module.connectDdb = async (region, endpoint) => {
        const payload = { region };
        if (endpoint) {
            payload.endpoint = endpoint;
        }
        const r = await proxyRequest("/connect", {
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

    // Data-plane forward: POST the AWS-JSON body to /ddb. A 2xx returns the body
    // verbatim; a DynamoDB service error (4xx/5xx carrying a `__type` document)
    // is returned to the codec to map to the faithful ddbsh error string; a
    // proxy/transport error throws (a transport-level failure for the engine).
    let fetchCalls = 0;
    Module.fetchDdb = async (op, body) => {
        fetchCalls += 1;
        const r = await proxyRequest("/ddb", {
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
                return r.text; // DynamoDB service error -> codec maps it
            }
        } catch {
            // fall through to a transport-level failure
        }
        throw new Error(`proxy /ddb HTTP ${r.status}: ${r.text.slice(0, 200)}`);
    };

    const session = new Module.DdbshSession(REGION);

    console.log(`P6f-b WASM smoke — ddbsh FETCH edition over the REAL ddbsh-web-proxy`);
    console.log(`  proxy:  ${PROXY_ORIGIN}`);
    console.log(`  region: ${REGION}`);
    console.log(`  table:  ${TABLE}`);

    let ok = true;
    try {
        // 0) SHOW CONNECTION — descriptor seam, no backend call.
        let r = await run(session, "show connection");
        check(r.ok, "show connection ok");
        contains(r.stdout, "wire/fetch -> POST /ddb (proxy)",
                 "show connection reports the FetchTransport backend binding");

        // 1) CONNECT — drives FetchTransport::on_connect -> Module.connectDdb ->
        //    the proxy's /connect, establishing this session's signing target.
        r = await run(session, `connect ${REGION}`);
        check(r.ok, "connect ok");
        contains(r.stdout, "CONNECT", "connect status token");

        // 2) CREATE TABLE (waits for ACTIVE via DescribeTable through the proxy).
        r = await run(session,
            `create table ${TABLE} ( p number, s number ) ` +
            "primary key ( p hash, s range ) billing mode on demand");
        check(r.ok, "create table ok");
        contains(r.stdout, "CREATE", "create table status token");

        // 3) INSERT three items.
        r = await run(session,
            `insert into ${TABLE} (p, s, v) values (1, 2, 100), (1, 3, 200), (2, 9, 300)`);
        check(r.ok, "insert ok");
        contains(r.stdout, "INSERT", "insert status token");

        // 4) SELECT reducing to GetItem (full composite key).
        r = await run(session, `select * from ${TABLE} where p = 1 and s = 2`);
        check(r.ok, "select GetItem ok");
        contains(r.stdout, "{p: 1, s: 2, v: 100}", "GetItem item render");

        // 5) SELECT reducing to Query (hash only).
        r = await run(session, `select * from ${TABLE} where p = 1`);
        check(r.ok, "select Query ok");
        contains(r.stdout, "{p: 1, s: 2, v: 100}", "Query item 1 render");
        contains(r.stdout, "{p: 1, s: 3, v: 200}", "Query item 2 render");

        // 6) EXPLAIN (resolves key schema via DescribeTable through the proxy).
        r = await run(session, `explain select * from ${TABLE} where p = 1 and s = 2`);
        check(r.ok, "explain GetItem ok");
        contains(r.stdout, "GetItem(", "explain GetItem request");
        r = await run(session, `explain select * from ${TABLE} where p = 1`);
        check(r.ok, "explain Query ok");
        contains(r.stdout, "Query(", "explain Query request");

        // 7) UPDATE a single item, then confirm the new value.
        r = await run(session, `update ${TABLE} set v = 999 where p = 1 and s = 2`);
        check(r.ok, "update ok");
        contains(r.stdout, "UPDATE", "update status token");
        r = await run(session, `select * from ${TABLE} where p = 1 and s = 2`);
        contains(r.stdout, "{p: 1, s: 2, v: 999}", "updated value read back");

        // 8) DELETE a single item.
        r = await run(session, `delete from ${TABLE} where p = 2 and s = 9`);
        check(r.ok, "delete ok");
        contains(r.stdout, "DELETE", "delete status token");

        // 9) ccf: a conditional INSERT over an existing key fails; DynamoDB's
        //    ConditionalCheckFailed `__type` document is mapped by the codec.
        r = await run(session, `insert into ${TABLE} (p, s, v) values (1, 2, 1)`);
        check(!r.ok, "duplicate insert fails (not ok)");
        contains(r.stderr, "The conditional request failed", "ccf error mapped to ddbsh string");
    } catch (e) {
        console.error("P6f-b smoke: ERROR during corpus", e);
        ok = false;
    } finally {
        // Teardown: drop the table (best-effort, even on failure) so no real
        // table is leaked. DROP waits for the table to be gone.
        try {
            const r = await run(session, `drop table if exists ${TABLE} nowait`);
            console.log(`  (teardown) drop ${TABLE}: ${r.ok ? "ok" : "failed"}`);
        } catch (e) {
            console.error(`  (teardown) drop ${TABLE} ERROR`, e);
        }
    }

    check(fetchCalls > 0, `Module.fetchDdb was exercised (${fetchCalls} calls)`);

    const pass = ok && failures === 0;
    console.log(pass
        ? "\nP6f-b WASM proxy smoke: PASS"
        : `\nP6f-b WASM proxy smoke: FAIL (${failures} assertion failure(s))`);
    process.exit(pass ? 0 : 1);
}

main().catch((e) => {
    console.error("P6f-b WASM proxy smoke: ERROR", e);
    process.exit(2);
});
