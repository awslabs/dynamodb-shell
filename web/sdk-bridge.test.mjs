// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// sdk-bridge.test.mjs — offline Node tests for web/sdk-bridge.mjs (P9 §4.5).
//
// Runs the REAL vendored AWS SDK bundle with a stub `requestHandler` (an SDK
// config hook), so the middleware placement, signing, retry and raw-body
// capture are exercised exactly as in the browser, with no network and no AWS
// account. Zero dependencies: plain asserts + an exit code. Needs Node >= 20
// (the SDK's floor); run with `node web/sdk-bridge.test.mjs` (emsdk's Node) or
// via devtools/run-web-smoke.

import * as sdk from "./vendor/aws-sdk/aws-sdk-dynamodb.mjs";
import { ALLOWED_HOSTS, REGIONS, SDK_VERSION } from "./vendor/aws-sdk/allowed-hosts.mjs";
import { OPERATIONS, createSdkBridge, installHooks } from "./sdk-bridge.mjs";

let failures = 0;
function check(cond, label) {
    if (cond) {
        console.log(`  ok   ${label}`);
    } else {
        console.error(`  FAIL ${label}`);
        failures += 1;
    }
}
function eq(actual, expected, label) {
    const a = JSON.stringify(actual);
    const e = JSON.stringify(expected);
    check(a === e, `${label} — expected ${e}, got ${a}`);
}
async function rejects(promise, re, label) {
    try {
        await promise;
        check(false, `${label} — expected a rejection`);
    } catch (e) {
        check(re.test(e.message), `${label} — message: ${e.message}`);
    }
}

// Test fixtures. Every value below is fictitious: none was ever issued by AWS
// and none was ever a valid credential. The key ids are the alphabet
// (ABCDEFGHIJKLMNOP) behind the ASIA/AKIA prefixes, and SECRET and TOKEN are
// invented. Every expiry, and every clock (NOW) a test injects, is in 2025, so
// a value mistaken for a real one would be long expired.
const KEY = "ASIAABCDEFGHIJKLMNOP";           // short-term shape (accepted)
const LONG_TERM_KEY = "AKIAABCDEFGHIJKLMNOP"; // long-term shape (refused)
const SECRET = "s3cr3t/SECRETVALUE+xyz";
const TOKEN = "TOKENVALUE-session-token-abc";
const CREDS = { accessKeyId: KEY, secretAccessKey: SECRET, sessionToken: TOKEN };
const NOW = Date.parse("2025-01-15T12:00:00Z");
const EXP = "2025-01-15T13:00:00Z"; // one hour after NOW

// A stub HTTP handler: replays scripted responses and records each request.
function stub(script) {
    const calls = [];
    const handler = {
        async handle(request) {
            calls.push({
                hostname: request.hostname,
                target: request.headers["x-amz-target"],
                auth: request.headers["authorization"] || "",
                token: request.headers["x-amz-security-token"] || "",
                body: request.body,
            });
            const next = script.length > 1 ? script.shift() : script[0];
            if (next instanceof Error) {
                throw next;
            }
            return {
                response: {
                    statusCode: next.status,
                    headers: { "content-type": "application/x-amz-json-1.0" },
                    body: new Response(next.body).body,
                },
            };
        },
        updateHttpClientConfig() {},
        httpHandlerConfigs() {
            return {};
        },
    };
    return { calls, handler };
}

function bridgeWith(script, extra = {}) {
    const s = stub(script);
    const bridge = createSdkBridge({
        sdk,
        allowedHosts: ALLOWED_HOSTS,
        regions: REGIONS,
        sdkVersion: SDK_VERSION,
        clientConfig: { requestHandler: s.handler },
        ...extra,
    });
    return { bridge, calls: s.calls };
}

// ---- operations allowlist matches the bundle --------------------------------
console.log("operations");
eq(OPERATIONS.length, 26, "26 codec operations");
check(OPERATIONS.every((op) => typeof sdk[`${op}Command`] === "function"),
    "every OPERATIONS entry has a command class in the bundle");
const bundled = Object.keys(sdk).filter((k) => k.endsWith("Command")).map((k) => k.slice(0, -7)).sort();
eq(bundled, [...OPERATIONS].sort(), "the bundle exports exactly the OPERATIONS commands");

// ---- body passthrough + target + raw success body ---------------------------
console.log("send: passthrough");
{
    const ok = '{"TableNames":["t1"],"Extra":{"Unmodelled":true}}';
    const { bridge, calls } = bridgeWith([{ status: 200, body: ok }]);
    bridge.setCredentials(CREDS);
    const body = '{"Limit":2,"ExclusiveStartTableName":"héllo — 日本 😀"}';
    const raw = await bridge.send("ListTables", body);
    eq(raw, ok, "success body returned verbatim (unmodelled members kept)");
    eq(calls.length, 1, "one HTTP request");
    eq(calls[0].body, body, "request body is the engine's bytes (non-ASCII intact)");
    eq(calls[0].target, "DynamoDB_20120810.ListTables", "x-amz-target set from the operation");
    eq(calls[0].hostname, "dynamodb.us-east-1.amazonaws.com", "default regional host");
    check(calls[0].auth.startsWith("AWS4-HMAC-SHA256 Credential=" + KEY + "/"), "request is SigV4-signed");
    eq(calls[0].token, TOKEN, "session token header sent");
}
{
    // The SDK's endpoint rules route SearchVectors to search-dynamodb.<region>;
    // that host is in ALLOWED_HOSTS, so vector search is not refused.
    const { bridge, calls } = bridgeWith([{ status: 200, body: '{"Items":[]}' }]);
    bridge.setCredentials(CREDS);
    eq(await bridge.send("SearchVectors", '{"TableName":"t"}'), '{"Items":[]}', "SearchVectors body returned");
    eq(calls[0].hostname, "search-dynamodb.us-east-1.amazonaws.com", "SearchVectors goes to the search host");
    eq(calls[0].target, "DynamoDB_20120810.SearchVectors", "SearchVectors target");
}

// ---- __type error document returned verbatim --------------------------------
console.log("send: service error");
{
    const err = '{"__type":"com.amazonaws.dynamodb.v20120810#ResourceNotFoundException",' +
        '"message":"Requested resource not found: Table: nope not found"}';
    const { bridge } = bridgeWith([{ status: 400, body: err }]);
    bridge.setCredentials(CREDS);
    eq(await bridge.send("DescribeTable", '{"TableName":"nope"}'), err, "400 __type body returned verbatim");
}

// ---- retry owned by the SDK -------------------------------------------------
console.log("send: retry");
{
    const thr = '{"__type":"com.amazonaws.dynamodb.v20120810#ThrottlingException","message":"slow down"}';
    const ok = '{"Table":{"TableName":"t"}}';
    const { bridge, calls } = bridgeWith([{ status: 400, body: thr }, { status: 400, body: thr },
        { status: 200, body: ok }]);
    bridge.setCredentials(CREDS);
    eq(await bridge.send("DescribeTable", '{"TableName":"t"}'), ok, "final body after throttling retries");
    eq(calls.length, 3, "three attempts (two throttled, one success)");
}

// ---- transport failure -------------------------------------------------------
console.log("send: transport failure");
{
    const { bridge } = bridgeWith([new Error("network down")]);
    bridge.setCredentials(CREDS);
    await rejects(bridge.send("ListTables", "{}"), /network down/, "no HTTP response rejects with the message");
}

// ---- refusals before any request ----------------------------------------------
console.log("send: refusals");
{
    const { bridge, calls } = bridgeWith([{ status: 200, body: "{}" }]);
    await rejects(bridge.send("ListTables", "{}"), /no credentials/, "no credentials");
    bridge.setCredentials(CREDS);
    await rejects(bridge.send("BatchWriteItem", "{}"), /not supported/, "unknown operation");
    await rejects(bridge.send("constructor", "{}"), /not supported/, "prototype name is not an operation");
    eq(calls.length, 0, "no request was made");
}
{
    let t = NOW;
    const { bridge, calls } = bridgeWith([{ status: 200, body: "{}" }], { now: () => t });
    const problems = [];
    bridge.onCredentialProblem((p) => problems.push(p.kind));
    const r = bridge.setCredentials({ ...CREDS, expiration: new Date(EXP) });
    check(r.ok === true, "unexpired credentials accepted");
    t = Date.parse("2025-01-15T13:00:01Z");
    await rejects(bridge.send("ListTables", "{}"), /credentials expired at 2025-01-15T13:00:00.000Z/,
        "locally expired credentials refused");
    eq(problems, ["expired"], "expiry callback fired");
    eq(calls.length, 0, "no request with expired credentials");
}
{
    const expired = '{"__type":"com.amazon.coral.service#ExpiredTokenException","message":"The security token included in the request is expired"}';
    const { bridge } = bridgeWith([{ status: 400, body: expired }]);
    const problems = [];
    bridge.onCredentialProblem((p) => problems.push(`${p.kind}:${p.detail}`));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("ListTables", "{}"), expired, "ExpiredTokenException body still returned");
    eq(problems, ["rejected:ExpiredTokenException"], "credential-problem callback fired");
}
{
    const { bridge } = bridgeWith([{ status: 200, body: "{}" }]);
    const r = bridge.setCredentials({ accessKeyId: LONG_TERM_KEY, secretAccessKey: "x", sessionToken: "y" });
    check(r.ok === false && /long-term/.test(r.message), "setCredentials re-validates (AKIA refused)");
}

// ---- CONNECT validation (§5.2) ------------------------------------------------
console.log("connect");
{
    const { bridge, calls } = bridgeWith([{ status: 200, body: "{}" }]);
    bridge.setCredentials(CREDS);
    eq(bridge.connect("us-west-2", ""), "", "plain region accepted");
    await bridge.send("ListTables", "{}");
    eq(calls[calls.length - 1].hostname, "dynamodb.us-west-2.amazonaws.com", "client rebuilt for the region");
    for (const ep of ["https://dynamodb-fips.us-east-1.amazonaws.com",
        "https://dynamodb.us-east-1.api.aws", "https://dynamodb-fips.us-east-1.api.aws",
        "https://dynamodb.us-east-1.amazonaws.com/", "https://dynamodb.us-east-1.amazonaws.com:443",
        "https://DYNAMODB.us-east-1.amazonaws.com"]) {
        eq(bridge.connect("us-east-1", ep), "", `accepted: ${ep}`);
    }
    await bridge.send("ListTables", "{}");
    eq(calls[calls.length - 1].hostname, "dynamodb.us-east-1.amazonaws.com", "endpoint override used");
    eq(bridge.connect("us-gov-west-1", ""), "", "aws-us-gov region accepted");

    const denied = [
        ["us-east-1", "http://dynamodb.us-east-1.amazonaws.com", /https:\/\/ is required/],
        ["us-east-1", "https://mybucket.s3.amazonaws.com", /not an AWS DynamoDB endpoint/],
        ["us-east-1", "https://evil.example.com", /not an AWS DynamoDB endpoint/],
        ["us-east-1", "https://localhost:8000", /only the default port/],
        ["us-east-1", "https://user:pw@dynamodb.us-east-1.amazonaws.com", /user information/],
        ["us-east-1", "https://dynamodb.us-east-1.amazonaws.com:8443", /only the default port/],
        ["us-east-1", "https://dynamodb.us-east-1.amazonaws.com/x", /a path is not allowed/],
        ["us-east-1", "https://dynamodb.us-east-1.amazonaws.com/?a=b", /query or fragment/],
        ["us-east-1", "https://dynamodb.us-east-1.amazonaws.com/#f", /query or fragment/],
        ["us-east-1", "https://dynamodb.us-east-2.amazonaws.com", /in us-east-2, not us-east-1/],
        ["us-east-1", "https://dynamodb.us-east-1.amazonaws.com.", /not an AWS DynamoDB endpoint/],
        ["us-east-1", "https://search-dynamodb.us-east-1.amazonaws.com", /not an AWS DynamoDB endpoint/],
        ["us-east-1", "not a url", /not a valid URL/],
        ["mars-north-1", "", /not a known AWS region/],
        ["cn-north-1", "", /not a known AWS region/],
        ["US-EAST-1", "", /not a known AWS region/],
    ];
    for (const [r, ep, re] of denied) {
        const msg = bridge.connect(r, ep);
        check(re.test(msg), `denied: ${r} ${ep} — ${msg}`);
    }
    const n = calls.length;
    await rejects(bridge.send("ListTables", "{}"), /CONNECT rejected: region US-EAST-1/,
        "a rejected CONNECT blocks later sends (fail-closed)");
    eq(calls.length, n, "no request while blocked");
    check(bridge.descriptor().includes("blocked: region US-EAST-1"), "descriptor shows the blocked state");
    eq(bridge.connect("us-east-1", ""), "", "a valid CONNECT clears the block");
    await bridge.send("ListTables", "{}");
    eq(calls.length, n + 1, "sends resume after a valid CONNECT");
}

// ---- hostname check in the request middleware --------------------------------
console.log("middleware host check");
{
    // An allowlist that excludes the SDK's resolved host: the middleware must
    // refuse even though connect() was never asked about it.
    const s = stub([{ status: 200, body: "{}" }]);
    const bridge = createSdkBridge({
        sdk, allowedHosts: ["dynamodb.us-west-2.amazonaws.com"], regions: REGIONS,
        sdkVersion: SDK_VERSION, clientConfig: { requestHandler: s.handler },
    });
    bridge.setCredentials(CREDS);
    await rejects(bridge.send("ListTables", "{}"), /endpoint dynamodb.us-east-1.amazonaws.com is not permitted/,
        "resolved host outside allowedHosts refused");
    eq(s.calls.length, 0, "no request to a non-allowlisted host");
}

// ---- descriptor + secrecy -----------------------------------------------------
console.log("descriptor");
{
    // A fixed 2025 clock: with the real clock (bridgeWith's default Date.now) a
    // 2025 expiry would be refused as already expired.
    const { bridge } = bridgeWith([{ status: 200, body: "{}" }], { now: () => NOW });
    eq(bridge.descriptor(),
        `wire/aws-sdk-js ${SDK_VERSION} -> https://dynamodb.us-east-1.amazonaws.com (default); credentials none`,
        "descriptor without credentials");
    const set = bridge.setCredentials({ ...CREDS, expiration: new Date(EXP) });
    check(set.ok === true, "fixture credentials accepted under the injected clock");
    bridge.connect("us-east-1", "https://dynamodb-fips.us-east-1.amazonaws.com");
    const d = bridge.descriptor();
    eq(d, `wire/aws-sdk-js ${SDK_VERSION} -> https://dynamodb-fips.us-east-1.amazonaws.com; ` +
        "credentials ASIA…MNOP expires 2025-01-15T13:00:00.000Z", "descriptor with credentials");
    check(!d.includes(SECRET) && !d.includes(TOKEN) && !d.includes(KEY), "descriptor reveals no secret, token or full key id");
    const all = [d, JSON.stringify(bridge.credentialSummary()), bridge.connect("x", "")].join("\n");
    check(!all.includes(SECRET) && !all.includes(TOKEN), "summaries and messages reveal no secret or token");
    bridge.clearCredentials();
    check(bridge.descriptor().endsWith("credentials none"), "clearCredentials forgets the set");
}

// ---- installHooks puts only functions on Module ------------------------------
console.log("installHooks");
{
    const { bridge } = bridgeWith([{ status: 200, body: "{}" }]);
    bridge.setCredentials(CREDS);
    const Module = {};
    installHooks(Module, bridge);
    eq(Object.keys(Module).sort(), ["ddbshConnect", "ddbshDescriptor", "ddbshSend"], "three hooks installed");
    check(Object.values(Module).every((v) => typeof v === "function"), "hooks are functions only");
    check(!JSON.stringify(Module).includes(SECRET), "no credential reachable from Module");
}

if (failures > 0) {
    console.error(`\nsdk-bridge tests: ${failures} FAILED`);
    process.exit(1);
}
console.log("\nsdk-bridge tests: all passed");
