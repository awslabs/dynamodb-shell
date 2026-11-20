// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// sdk-bridge.mjs — the browser edition's bridge from the ddbsh engine (WASM) to
// the AWS SDK for JavaScript v3 (P9, D-P9-02-1 / D-P9-02-2).
//
// The engine serializes every DynamoDB request itself (ddbsh-wire) and parses
// every response itself. The SDK is used as a SIGNED HTTP PIPE: it supplies
// credential handling, endpoint resolution, SigV4, retries (D-P9-02-3),
// clock-skew correction and fetch. Two per-command middlewares swap the
// engine's AWS-JSON-1.0 body into the request after the SDK's serializer, and
// capture the raw response body before the SDK's deserializer, so no
// SDK-shaped object ever reaches the engine and no output normalization is
// needed. A service error arrives as its verbatim `__type` document, which the
// engine's parser maps exactly as on every other wire path.
//
// DOM-free: imported by both the page (app.js) and the Node golden shim
// (ddbsh-node.mjs), so the page and the fidelity gate run the same code
// (D-P9-02-4). Credentials live only in this module's closure and the SDK
// client's config (D-P9-4): never on `window` or `Module`, never logged, never
// stored.
//
// The JS hooks the WASM transport calls (web/js_sdk_transport.cpp):
//   Module.ddbshSend(op, body)        -> Promise<string>   bridge.send
//   Module.ddbshConnect(region, url)  -> string ("" = ok)  bridge.connect
//   Module.ddbshDescriptor()          -> string            bridge.descriptor
// installHooks(Module, bridge) wires them.

import { maskAccessKeyId, validateCredentials } from "./app-core.mjs";

/** The operations the ddbsh-wire codec emits (an allowlist; see entry.mjs). */
export const OPERATIONS = Object.freeze([
    "CreateBackup",
    "CreateTable",
    "DeleteBackup",
    "DeleteItem",
    "DeleteTable",
    "DescribeBackup",
    "DescribeContinuousBackups",
    "DescribeLimits",
    "DescribeTable",
    "DescribeTableReplicaAutoScaling",
    "DescribeTimeToLive",
    "GetItem",
    "ListBackups",
    "ListTables",
    "PutItem",
    "Query",
    "RestoreTableFromBackup",
    "RestoreTableToPointInTime",
    "Scan",
    "SearchVectors",
    "TransactGetItems",
    "TransactWriteItems",
    "UpdateContinuousBackups",
    "UpdateItem",
    "UpdateTable",
    "UpdateTimeToLive",
]);

const REGION_SHAPE = /^[a-z]{2}(-[a-z]+)+-\d+$/;
const CREDENTIAL_PROBLEM_TYPES = ["ExpiredTokenException", "UnrecognizedClientException"];

const ALLOWED_FORMS = "https://dynamodb.<region>.amazonaws.com, " +
    "https://dynamodb-fips.<region>.amazonaws.com, https://dynamodb.<region>.api.aws, " +
    "https://dynamodb-fips.<region>.api.aws";

/**
 * Create the bridge.
 *
 * @param {object} opts
 * @param {object} opts.sdk            the vendored SDK module namespace
 *                                     (DynamoDBClient + the <Op>Command classes).
 * @param {string[]} opts.allowedHosts the permitted DynamoDB hostnames.
 * @param {string[]} opts.regions      the permitted region names.
 * @param {string} opts.sdkVersion     the bundled SDK version (SHOW CONNECTION).
 * @param {string} [opts.region]       the initial region (default us-east-1).
 * @param {() => number} [opts.now]    clock (ms since epoch); injectable for tests.
 * @param {object} [opts.clientConfig] extra DynamoDBClient config merged last
 *                                     (tests pass a stub `requestHandler`).
 */
export function createSdkBridge({
    sdk,
    allowedHosts,
    regions,
    sdkVersion,
    region = "us-east-1",
    now = Date.now,
    clientConfig = {},
}) {
    const hostSet = new Set(allowedHosts);
    const regionSet = new Set(regions);
    const opSet = new Set(OPERATIONS);

    let credentials = null; // {accessKeyId, secretAccessKey, sessionToken, expiration?}
    let currentRegion = region;
    let currentEndpoint = ""; // normalized origin, or "" for SDK default resolution
    let blocked = null;       // reason string while a rejected CONNECT is in force
    let client = null;
    const problemListeners = [];

    function notifyProblem(kind, detail) {
        for (const cb of problemListeners) {
            try {
                cb({ kind, detail });
            } catch {
                // a UI listener failure must never break a send.
            }
        }
    }

    function rebuildClient() {
        client = null;
        if (credentials === null || blocked !== null) {
            return;
        }
        const creds = {
            accessKeyId: credentials.accessKeyId,
            secretAccessKey: credentials.secretAccessKey,
            sessionToken: credentials.sessionToken,
        };
        if (credentials.expiration instanceof Date) {
            creds.expiration = credentials.expiration;
        }
        client = new sdk.DynamoDBClient({
            region: currentRegion,
            endpoint: currentEndpoint || undefined,
            credentials: creds,
            accountIdEndpointMode: "disabled",
            maxAttempts: 11,
            retryMode: "standard",
            ...clientConfig,
        });
    }

    // Validate CONNECT's region and optional endpoint (§5.2). Returns
    // {ok:true, endpoint} with the normalized origin, or {ok:false, reason}.
    function checkTarget(r, endpoint) {
        if (typeof r !== "string" || !REGION_SHAPE.test(r) || !regionSet.has(r)) {
            return { ok: false, reason: `region ${r} is not a known AWS region for the browser edition` };
        }
        if (typeof endpoint !== "string" || endpoint === "") {
            return { ok: true, endpoint: "" };
        }
        let url;
        try {
            url = new URL(endpoint);
        } catch {
            return { ok: false, reason: `endpoint ${endpoint} is not a valid URL; allowed forms: ${ALLOWED_FORMS}` };
        }
        const fail = (rule) => ({
            ok: false,
            reason: `endpoint ${endpoint} is not permitted in the browser edition (${rule}); allowed forms: ${ALLOWED_FORMS}`,
        });
        if (url.protocol !== "https:") {
            return fail("https:// is required");
        }
        if (url.username !== "" || url.password !== "") {
            return fail("user information is not allowed");
        }
        if (url.port !== "" && url.port !== "443") {
            return fail("only the default port is allowed");
        }
        if (url.pathname !== "/" && url.pathname !== "") {
            return fail("a path is not allowed");
        }
        if (url.search !== "" || url.hash !== "") {
            return fail("a query or fragment is not allowed");
        }
        if (!hostSet.has(url.hostname)) {
            return fail("not an AWS DynamoDB endpoint");
        }
        const hostRegion = url.hostname.split(".")[1];
        if (hostRegion !== r) {
            return fail(`the endpoint is in ${hostRegion}, not ${r}`);
        }
        return { ok: true, endpoint: `https://${url.hostname}` };
    }

    function addRawBodyMiddleware(c, cmd, op, body, holder) {
        cmd.middlewareStack.addRelativeTo(
            (next) => async (args) => {
                const host = args.request && args.request.hostname;
                if (!hostSet.has(host)) {
                    throw new Error(`endpoint ${host} is not permitted in the browser edition`);
                }
                args.request.body = body;
                args.request.headers["x-amz-target"] = `DynamoDB_20120810.${op}`;
                return next(args);
            },
            { name: "ddbshRawRequest", toMiddleware: "serializerMiddleware", relation: "after", override: true },
        );
        cmd.middlewareStack.addRelativeTo(
            (next) => async (args) => {
                // Per attempt: a retried attempt that gets no HTTP response must
                // not resolve with an earlier attempt's body.
                holder.raw = undefined;
                const out = await next(args);
                const bytes = await c.config.streamCollector(out.response.body);
                holder.raw = new TextDecoder("utf-8").decode(bytes);
                out.response.body = bytes; // the SDK's deserializer + retry still run.
                return out;
            },
            { name: "ddbshRawResponse", toMiddleware: "deserializerMiddleware", relation: "after", override: true },
        );
    }

    function errorType(raw) {
        try {
            const t = JSON.parse(raw).__type;
            return typeof t === "string" ? t.slice(t.lastIndexOf("#") + 1) : "";
        } catch {
            return "";
        }
    }

    return {
        /**
         * Install a validated credential set (the output of parseCredentials).
         * Re-validates (defense in depth) and rebuilds the client.
         * @returns {{ok: true, expiration?: Date} | {ok: false, message: string}}
         */
        setCredentials(c) {
            const problem = validateCredentials(c, now());
            if (problem !== null) {
                return { ok: false, message: problem };
            }
            credentials = {
                accessKeyId: c.accessKeyId,
                secretAccessKey: c.secretAccessKey,
                sessionToken: c.sessionToken,
            };
            if (c.expiration instanceof Date) {
                credentials.expiration = c.expiration;
            }
            rebuildClient();
            return credentials.expiration ? { ok: true, expiration: credentials.expiration } : { ok: true };
        },

        /** Drop the credentials and the client. */
        clearCredentials() {
            credentials = null;
            client = null;
        },

        /** The masked key id + expiration, or null; never the secret or token. */
        credentialSummary() {
            if (credentials === null) {
                return null;
            }
            return credentials.expiration
                ? { accessKeyId: credentials.accessKeyId, expiration: credentials.expiration }
                : { accessKeyId: credentials.accessKeyId };
        },

        /**
         * CONNECT <region> [WITH ENDPOINT <url>] (§5.2, D-P9-02-6). Returns ""
         * on success; on rejection returns the reason, drops the client, and
         * blocks every later send until a valid CONNECT.
         */
        connect(r, endpoint) {
            const t = checkTarget(r, endpoint);
            if (!t.ok) {
                blocked = t.reason;
                client = null;
                return t.reason;
            }
            blocked = null;
            currentRegion = r;
            currentEndpoint = t.endpoint;
            rebuildClient();
            return "";
        },

        /**
         * Send one AWS-JSON-1.0 request; resolve with the raw response body
         * (success document or `__type` error document). Rejects only when no
         * HTTP response was received (or the bridge refuses to send).
         */
        async send(op, body) {
            if (!opSet.has(op)) {
                throw new Error(`operation ${op} is not supported by the browser edition`);
            }
            if (blocked !== null) {
                throw new Error(`CONNECT rejected: ${blocked}`);
            }
            if (credentials === null) {
                throw new Error("no credentials: paste short-term AWS credentials in the credentials panel");
            }
            if (credentials.expiration && now() >= credentials.expiration.getTime()) {
                const at = credentials.expiration.toISOString();
                notifyProblem("expired", at);
                throw new Error(`credentials expired at ${at}: paste new short-term credentials`);
            }
            if (client === null) {
                rebuildClient();
            }
            const c = client;
            const cmd = new sdk[`${op}Command`]({});
            const holder = { raw: undefined };
            addRawBodyMiddleware(c, cmd, op, body, holder);
            try {
                await c.send(cmd);
            } catch (e) {
                if (holder.raw === undefined) {
                    throw new Error(e && e.message ? e.message : String(e));
                }
            }
            const type = errorType(holder.raw);
            if (CREDENTIAL_PROBLEM_TYPES.includes(type)) {
                notifyProblem("rejected", type);
            }
            return holder.raw;
        },

        /** The SHOW CONNECTION backend line (§5.4). Never a secret or token. */
        descriptor() {
            let target;
            if (blocked !== null) {
                target = `blocked: ${blocked}`;
            } else if (currentEndpoint !== "") {
                target = currentEndpoint;
            } else {
                target = `https://dynamodb.${currentRegion}.amazonaws.com (default)`;
            }
            let cred = "credentials none";
            if (credentials !== null) {
                cred = `credentials ${maskAccessKeyId(credentials.accessKeyId)}`;
                if (credentials.expiration) {
                    cred += ` expires ${credentials.expiration.toISOString()}`;
                }
            }
            return `wire/aws-sdk-js ${sdkVersion} -> ${target}; ${cred}`;
        },

        /** The reason a rejected CONNECT is blocking sends, or null. */
        blockedReason() {
            return blocked;
        },

        /** Register a UI callback for expired / rejected credentials. */
        onCredentialProblem(cb) {
            if (typeof cb === "function") {
                problemListeners.push(cb);
            }
        },
    };
}

/**
 * Wire the three WASM transport hooks on an instantiated Emscripten Module to a
 * bridge. Only the hook functions are placed on Module; the bridge (and the
 * credentials it closes over) is not.
 */
export function installHooks(Module, bridge) {
    Module.ddbshSend = (op, body) => bridge.send(op, body);
    Module.ddbshConnect = (region, endpoint) => bridge.connect(region, endpoint);
    Module.ddbshDescriptor = () => bridge.descriptor();
}
