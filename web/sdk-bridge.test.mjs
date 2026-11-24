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
                sentAt: Date.now(),
                hostname: request.hostname,
                target: request.headers["x-amz-target"],
                auth: request.headers["authorization"] || "",
                token: request.headers["x-amz-security-token"] || "",
                userAgent: request.headers["x-amz-user-agent"] || request.headers["user-agent"] || "",
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
    const events = [];
    bridge.onRetryEvent((e) => events.push({ ...e, at: calls.length }));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("DescribeTable", '{"TableName":"t"}'), ok, "final body after throttling retries");
    eq(calls.length, 3, "three attempts (two throttled, one success)");
    eq(events.map((e) => e.type), ["retry", "retry", "settled"], "two retry events, then settled");
    eq(events.slice(0, 2).map((e) => [e.operation, e.attempt, e.maxAttempts, e.errorCode, e.throttled]),
        [["DescribeTable", 2, 11, "ThrottlingException", true],
            ["DescribeTable", 3, 11, "ThrottlingException", true]],
        "retry events: operation, attempt about to be made, budget, error code, throttled");
    eq(events.map((e) => e.at), [1, 2, 3], "each retry event is raised before its attempt is sent");
    check(events.slice(0, 2).every((e) => Number.isInteger(e.delayMs) && e.delayMs >= 0),
        "retry events carry the backoff delay in ms");
}

// Run fn with Math.random pinned, so the SDK's jittered backoff
// (floor(random * min(base * 2^retryCount, 20000)); in SDK 3.1174.0 the base is
// 1000 ms when throttled, else the DynamoDB client's defaultBaseDelay, 25 ms)
// is known.
async function withRandom(value, fn) {
    const saved = Math.random;
    Math.random = () => value;
    try {
        return await fn();
    } finally {
        Math.random = saved;
    }
}
// The browser's fetch failure, which the SDK classifies as transient (a
// network error with no HTTP response).
const fetchFailure = () => new TypeError("Failed to fetch");
// How far an event may lag the failed attempt, in ms (timer and event-loop
// slack; generous for slow CI runners: the regression it catches measured
// 901 ms, P12 review N12).
const TOLERANCE_MS = 300;
// How far a timer may fire before its delay, in ms. A lower bound ("attempt 2
// goes out the backoff after the event") uses this, not TOLERANCE_MS, so it
// stays meaningful for short delays such as the 22 ms 500 backoff (N12).
const EARLY_MS = 5;

// ---- retry events (P12, D-P12-18) -------------------------------------------
console.log("send: retry events");
await withRandom(0.9, async () => {
    // A 400 throttle, then a 200: one retry event, raised before the SDK
    // sleeps the 900 ms throttled backoff (0.9 x 1000), then settled.
    const thr = '{"__type":"com.amazonaws.dynamodb.v20120810#ProvisionedThroughputExceededException","message":"x"}';
    const { bridge, calls } = bridgeWith([{ status: 400, body: thr }, { status: 200, body: '{"Items":[]}' }]);
    const events = [];
    bridge.onRetryEvent((e) => events.push({ ...e, at: calls.length, raisedAt: Date.now() }));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("Scan", '{"TableName":"t"}'), '{"Items":[]}', "Scan succeeds after one throttle");
    eq(events.map((e) => e.type), ["retry", "settled"], "one retry event, then settled");
    eq([events[0].operation, events[0].attempt, events[0].errorCode, events[0].throttled, events[0].at],
        ["Scan", 2, "ProvisionedThroughputExceededException", true, 1],
        "the retry event names Scan, attempt 2, the throttling code, before attempt 2");
    eq(events[0].delayMs, 900, "the event carries the throttled backoff the SDK sleeps (0.9 x 1000 ms)");
    const lag = events[0].raisedAt - calls[0].sentAt;
    check(lag <= TOLERANCE_MS, `the event is raised promptly after the throttled response (${lag} ms)`);
    const wait = calls[1].sentAt - events[0].raisedAt;
    check(wait >= events[0].delayMs - EARLY_MS,
        `attempt 2 goes out the backoff after the event, so the event precedes the sleep (${wait} ms)`);
});
await withRandom(0.9, async () => {
    // A 500: the DynamoDB client's non-throttling base delay, 25 ms, gives 22 ms.
    const err = '{"__type":"com.amazonaws.dynamodb.v20120810#InternalServerError","message":"oops"}';
    const { bridge, calls } = bridgeWith([{ status: 500, body: err }, { status: 200, body: "{}" }]);
    const events = [];
    bridge.onRetryEvent((e) => events.push({ ...e, raisedAt: Date.now() }));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("ListTables", "{}"), "{}", "success after a 500 (pinned backoff)");
    eq([events[0].type, events[0].delayMs], ["retry", 22], "a 500 retry carries the base backoff (0.9 x 25 ms)");
    check(calls[1].sentAt - events[0].raisedAt >= events[0].delayMs - EARLY_MS,
        "attempt 2 after a 500 goes out the backoff after the event");
});
await withRandom(0.9, async () => {
    // Two throttles: the second backoff doubles (0.9 x 2000 = 1800 ms).
    const thr = '{"__type":"com.amazonaws.dynamodb.v20120810#ThrottlingException","message":"x"}';
    const { bridge } = bridgeWith([{ status: 400, body: thr }, { status: 400, body: thr },
        { status: 200, body: "{}" }]);
    const events = [];
    bridge.onRetryEvent((e) => events.push(e));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("ListTables", "{}"), "{}", "success after two throttles (pinned backoff)");
    eq(events.filter((e) => e.type === "retry").map((e) => [e.attempt, e.delayMs]), [[2, 900], [3, 1800]],
        "each retry carries its own backoff: 900 ms, then 1800 ms");
});
await withRandom(0.9, async () => {
    // No HTTP response, then a 200: a retry event with errorCode "" (R-S3-2).
    const { bridge, calls } = bridgeWith([fetchFailure(), { status: 200, body: "{}" }]);
    const events = [];
    bridge.onRetryEvent((e) => events.push(e));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("ListTables", "{}"), "{}", "success after a network failure");
    eq(calls.length, 2, "the network failure was retried");
    eq(events.map((e) => [e.type, e.operation, e.attempt, e.errorCode, e.throttled]),
        [["retry", "ListTables", 2, "", false], ["settled", undefined, undefined, undefined, undefined]],
        "a retry after no HTTP response has errorCode \"\" and is not throttled");
});
await withRandom(0.9, async () => {
    // A throttle, then no HTTP response, then a 200: the second event's code is
    // "", so the first attempt's body does not leak into it (the per-attempt
    // reset in addRawBodyMiddleware; R-S3-2).
    const thr = '{"__type":"com.amazonaws.dynamodb.v20120810#ThrottlingException","message":"x"}';
    const { bridge } = bridgeWith([{ status: 400, body: thr }, fetchFailure(),
        { status: 200, body: "{}" }]);
    const events = [];
    bridge.onRetryEvent((e) => events.push(e));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("ListTables", "{}"), "{}", "success after a throttle and a network failure");
    eq(events.filter((e) => e.type === "retry").map((e) => [e.attempt, e.errorCode, e.throttled]),
        [[2, "ThrottlingException", true], [3, "", false]],
        "the retry after the network failure does not report the earlier throttle");
});
{
    // Fallback: a strategy with no retryBackoffStrategy to hook. Two shapes.
    // (a) It sleeps inside refreshRetryTokenForRetry and records the delay in
    //     $retryLog (as SDK 3.1174.0 does): the event follows the sleep and
    //     carries the recorded delay.
    // (b) It returns the delay in getRetryDelay() and lets the middleware sleep
    //     it (older smithy): the event precedes the sleep with that delay.
    const token = (count, delay, logged) => ({
        getRetryCount: () => count,
        getRetryDelay: () => delay,
        getRetryCost: () => 1,
        isLongPoll: () => false,
        ...(logged === undefined ? {} : { $retryLog: { acquisitionDelay: logged } }),
    });
    const strategy = (sleepsItself) => ({
        mode: "standard",
        async acquireInitialRetryToken() {
            return token(0, 0);
        },
        async refreshRetryTokenForRetry(t) {
            if (sleepsItself) {
                await new Promise((r) => setTimeout(r, 120));
                return token(t.getRetryCount() + 1, 0, 120);
            }
            return token(t.getRetryCount() + 1, 130);
        },
        recordSuccess() {},
    });
    const thr = '{"__type":"com.amazonaws.dynamodb.v20120810#ThrottlingException","message":"x"}';
    for (const [sleepsItself, label] of [[true, "sleeps itself"], [false, "middleware sleeps"]]) {
        const s = stub([{ status: 400, body: thr }, { status: 200, body: "{}" }]);
        const fb = createSdkBridge({ sdk, allowedHosts: ALLOWED_HOSTS, regions: REGIONS, sdkVersion: SDK_VERSION,
            clientConfig: { requestHandler: s.handler, retryStrategy: strategy(sleepsItself) } });
        const events = [];
        fb.onRetryEvent((e) => events.push({ ...e, raisedAt: Date.now() }));
        fb.setCredentials(CREDS);
        eq(await fb.send("ListTables", "{}"), "{}", `fallback (${label}): the send succeeds`);
        eq(events.map((e) => [e.type, e.attempt, e.errorCode, e.throttled]),
            [["retry", 2, "ThrottlingException", true], ["settled", undefined, undefined, undefined]],
            `fallback (${label}): one retry event, then settled`);
        if (sleepsItself) {
            eq(events[0].delayMs, 120, "fallback (sleeps itself): the delay is $retryLog.acquisitionDelay");
            check(events[0].raisedAt - s.calls[0].sentAt >= 120 - EARLY_MS,
                "fallback (sleeps itself): the event follows the strategy's sleep");
        } else {
            eq(events[0].delayMs, 130, "fallback (middleware sleeps): the delay is getRetryDelay()");
            check(s.calls[1].sentAt - events[0].raisedAt >= 130 - EARLY_MS,
                "fallback (middleware sleeps): the middleware sleeps the delay after the event");
        }
    }
}
{
    // A non-retryable error: no retry event, only settled.
    const err = '{"__type":"com.amazonaws.dynamodb.v20120810#ResourceNotFoundException","message":"nope"}';
    const { bridge, calls } = bridgeWith([{ status: 400, body: err }]);
    const events = [];
    bridge.onRetryEvent((e) => events.push(e.type));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("DescribeTable", '{"TableName":"nope"}'), err, "non-retryable error body returned");
    eq(calls.length, 1, "one attempt");
    eq(events, ["settled"], "no retry event for a non-retryable error");
}
{
    // A retryable server error that is not throttling: throttled is false.
    const err = '{"__type":"com.amazonaws.dynamodb.v20120810#InternalServerError","message":"oops"}';
    const { bridge } = bridgeWith([{ status: 500, body: err }, { status: 200, body: "{}" }]);
    const events = [];
    bridge.onRetryEvent((e) => events.push(e));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("ListTables", "{}"), "{}", "success after a 500");
    eq(events.map((e) => [e.type, e.errorCode, e.throttled]),
        [["retry", "InternalServerError", false], ["settled", undefined, undefined]],
        "a 500 retry is reported as retrying, not throttled");
}
{
    // A code outside isThrottlingError's list that the SDK itself classifies
    // as throttling still counts (errorType THROTTLING), as the C++ decorator
    // honors ShouldThrottle().
    const err = '{"__type":"com.amazonaws.dynamodb.v20120810#RequestThrottled","message":"x"}';
    const { bridge } = bridgeWith([{ status: 400, body: err }, { status: 200, body: "{}" }]);
    const events = [];
    bridge.onRetryEvent((e) => events.push(e));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("ListTables", "{}"), "{}", "success after an SDK-classified throttle");
    eq([events[0].type, events[0].errorCode, events[0].throttled], ["retry", "RequestThrottled", true],
        "the SDK's own throttling classification sets throttled");
}
{
    // The budget runs out: no event for the last attempt, after which the SDK
    // gives up. maxAttempts 2 keeps the test short.
    const thr = '{"__type":"com.amazonaws.dynamodb.v20120810#ThrottlingException","message":"x"}';
    const s = stub([{ status: 400, body: thr }]);
    const short = createSdkBridge({ sdk, allowedHosts: ALLOWED_HOSTS, regions: REGIONS,
        sdkVersion: SDK_VERSION, clientConfig: { requestHandler: s.handler, maxAttempts: 2 } });
    const events = [];
    short.onRetryEvent((e) => events.push([e.type, e.attempt, e.maxAttempts]));
    short.setCredentials(CREDS);
    eq(await short.send("Query", "{}"), thr, "the last throttling body is returned when the budget runs out");
    eq(s.calls.length, 2, "two attempts (maxAttempts 2)");
    eq(events, [["retry", 2, 2], ["settled", undefined, undefined]],
        "one retry event for attempt 2 of 2; none after the last attempt");
}
{
    // A listener that throws breaks neither the send nor the retry.
    const thr = '{"__type":"com.amazonaws.dynamodb.v20120810#ThrottlingException","message":"x"}';
    const { bridge, calls } = bridgeWith([{ status: 400, body: thr }, { status: 200, body: "{}" }]);
    const seen = [];
    bridge.onRetryEvent(() => {
        throw new Error("listener bug");
    });
    bridge.onRetryEvent((e) => seen.push(e.type));
    bridge.setCredentials(CREDS);
    eq(await bridge.send("ListTables", "{}"), "{}", "send succeeds although a listener throws");
    eq(calls.length, 2, "the retry still happened");
    eq(seen, ["retry", "settled"], "later listeners still receive every event");
}
{
    // No HTTP response: a refusal before the SDK runs raises nothing; a
    // transport failure settles.
    const { bridge } = bridgeWith([new Error("network down")]);
    const events = [];
    bridge.onRetryEvent((e) => events.push(e.type));
    await rejects(bridge.send("ListTables", "{}"), /no credentials/, "refused without credentials");
    eq(events, [], "no event for a send refused before the SDK runs");
    bridge.setCredentials(CREDS);
    await rejects(bridge.send("ListTables", "{}"), /network down/, "transport failure rejects");
    eq(events[events.length - 1], "settled", "a rejected send settles");
}
{
    // The wrapped strategy keeps the SDK's retry mode: the user agent, which
    // reports it (feature E = standard retries), is the same as a plain
    // client's with the bridge's configuration.
    const { bridge, calls } = bridgeWith([{ status: 200, body: "{}" }]);
    bridge.setCredentials(CREDS);
    await bridge.send("ListTables", "{}");
    const s = stub([{ status: 200, body: "{}" }]);
    const plain = new sdk.DynamoDBClient({ region: "us-east-1", credentials: CREDS,
        accountIdEndpointMode: "disabled", maxAttempts: 11, retryMode: "standard",
        requestHandler: s.handler });
    await plain.send(new sdk.ListTablesCommand({}));
    check(calls[0].userAgent !== "" && calls[0].userAgent === s.calls[0].userAgent,
        `the user agent is unchanged by the wrapper (${calls[0].userAgent})`);
    check(/ m\/[^ ]*\bE\b/.test(calls[0].userAgent), "the user agent still reports standard retries (m/…E…)");
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
