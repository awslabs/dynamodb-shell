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
// Retry events (P12, D-P12-18): the bridge wraps the SDK client's retry
// strategy and tells onRetryEvent listeners about every retry the SDK has
// decided to make, and when each send settles. With the vendored SDK the
// retry event is raised before the backoff sleep and carries the delay about
// to be slept (see observingRetryStrategy for how, and for the fallback that
// raises it after the sleep with another SDK's strategy). The page turns them
// into the busy indicator's throttled / retrying states (app-core
// createBusyClock). They go straight to the page; nothing enters the engine.
//
// The JS hooks the WASM transport calls (web/js_sdk_transport.cpp):
//   Module.ddbshSend(op, body)        -> Promise<string>   bridge.send
//   Module.ddbshConnect(region, url)  -> string ("" = ok)  bridge.connect
//   Module.ddbshDescriptor()          -> string            bridge.descriptor
// installHooks(Module, bridge) wires them.

import { isThrottlingError, maskAccessKeyId, validateCredentials } from "./app-core.mjs";

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
// The per-object state observeBackoff keeps on a hooked backoff strategy.
const BACKOFF_STATE = Symbol("ddbshBackoffState");

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
    const retryListeners = [];
    // The sends between c.send(cmd) and its settling, oldest first:
    // {op, holder}. The engine issues one request at a time (it is suspended
    // in Asyncify until the request settles, and the page runs one submission
    // at a time), so there is at most one; a retry event names its operation
    // and error from it. With more than one, which send a retry belongs to is
    // unknown, and the event's operation is "".
    const inFlight = [];

    function notifyProblem(kind, detail) {
        for (const cb of problemListeners) {
            try {
                cb({ kind, detail });
            } catch {
                // a UI listener failure must never break a send.
            }
        }
    }

    function notifyRetryListeners(event) {
        for (const cb of retryListeners) {
            try {
                cb(event);
            } catch {
                // a UI listener failure must never break a send or a retry.
            }
        }
    }

    // The error code of the failed attempt a retry follows: the `__type` of
    // the response body the raw-response middleware captured for this attempt
    // (the same parse the engine applies), or the SDK error's name when the
    // send is unknown; "" when no HTTP response arrived (a network failure).
    function retryErrorCode(errorInfo, ctx) {
        const err = errorInfo && errorInfo.error;
        if (ctx !== null) {
            if (ctx.holder.raw === undefined) {
                return "";
            }
            return errorType(ctx.holder.raw) || (err && typeof err.name === "string" ? err.name : "");
        }
        const status = err && err.$metadata ? err.$metadata.httpStatusCode : undefined;
        return typeof status === "number" && typeof err.name === "string" ? err.name : "";
    }

    // Raise one retry event. `attempt` is the attempt about to be made (2 =
    // the first retry) and `delayMs` the backoff the SDK sleeps before it.
    function raiseRetry(errorInfo, attempt, budget, delayMs) {
        const ctx = inFlight.length === 1 ? inFlight[0] : null;
        const errorCode = retryErrorCode(errorInfo, ctx);
        notifyRetryListeners({
            type: "retry",
            operation: ctx !== null ? ctx.op : "",
            attempt,
            maxAttempts: budget,
            errorCode,
            throttled: isThrottlingError(errorCode) ||
                Boolean(errorInfo && errorInfo.errorType === "THROTTLING"),
            delayMs,
        });
    }

    // The delay StandardRetryStrategy sleeps for a computed backoff `c`: a
    // Retry-After hint (a Date on errorInfo) can lengthen it by up to 5 s.
    // This copies the SDK 3.1174.0 rule in refreshRetryTokenForRetry.
    function sleptDelay(c, errorInfo) {
        const hint = errorInfo && errorInfo.retryAfterHint;
        if (hint instanceof Date) {
            return Math.max(c, Math.min(hint.getTime() - Date.now(), c + 5000));
        }
        return c;
    }

    // Wrap a smithy retry strategy (RetryStrategyV2: acquireInitialRetryToken,
    // refreshRetryTokenForRetry, recordSuccess) so each retry it approves is
    // reported. Every other member delegates, `mode` included (the user agent
    // reports it). A strategy of another shape is returned as is.
    //
    // When the event is raised. In SDK 3.1174.0, StandardRetryStrategy's
    // refreshRetryTokenForRetry decides whether to retry, computes the backoff
    // with this.retryBackoffStrategy.computeNextBackoffDelay(retryCount),
    // SLEEPS the backoff itself, and only then returns a token whose
    // getRetryDelay() is 0 (the middleware's own sleep is then a no-op). An
    // event raised after it returns would come too late for the busy
    // indicator, and its delay would read 0. So:
    //   - Pre-sleep path (the page's case). The strategy calls
    //     computeNextBackoffDelay only when it will retry or the token is a
    //     long-poll token, and DynamoDB tokens are never long-poll (the path
    //     is not used for one that is). observeBackoff hooks that method on
    //     the strategy's retryBackoffStrategy, and the hook raises the event
    //     with the delay about to be slept, before the sleep starts.
    //   - Fallback (a strategy without that member: another SDK version, an
    //     adaptive strategy, a caller's strategy). The event is raised after
    //     refreshRetryTokenForRetry returns, with the delay the token records:
    //     $retryLog.acquisitionDelay when the strategy slept (then the event
    //     follows the sleep), else getRetryDelay() (then the middleware sleeps
    //     it after the event, as older smithy versions do).
    // refreshRetryTokenForRetry THROWS when the strategy will not retry (a
    // non-retryable error, the attempt budget spent, no retry capacity), so
    // neither path raises an event then.
    function observingRetryStrategy(inner, maxAttempts) {
        if (!inner || typeof inner.acquireInitialRetryToken !== "function" ||
            typeof inner.refreshRetryTokenForRetry !== "function" ||
            typeof inner.recordSuccess !== "function") {
            return inner;
        }
        const backoff = observeBackoff(inner.retryBackoffStrategy);
        return {
            get mode() {
                return inner.mode;
            },
            acquireInitialRetryToken(scope) {
                return inner.acquireInitialRetryToken(scope);
            },
            async refreshRetryTokenForRetry(token, errorInfo) {
                let budget = 0;
                try {
                    budget = Number(await maxAttempts()) || 0;
                } catch {
                    budget = 0; // the event says "unknown"; the retry still happens.
                }
                const retryCount = token.getRetryCount();
                const longPoll = typeof token.isLongPoll === "function" && token.isLongPoll();
                // One observer per backoff object at a time. A second retry
                // overlapping the first (none today: one request in flight)
                // takes the fallback path.
                const pending = { raised: false };
                const hooked = backoff !== null && !longPoll && backoff.observer === null;
                if (hooked) {
                    backoff.observer = (count, computed) => {
                        if (pending.raised || count !== retryCount) {
                            return;
                        }
                        pending.raised = true;
                        raiseRetry(errorInfo, retryCount + 2, budget, sleptDelay(computed, errorInfo));
                    };
                }
                let next;
                try {
                    next = await inner.refreshRetryTokenForRetry(token, errorInfo);
                } finally {
                    if (hooked) {
                        backoff.observer = null;
                    }
                }
                if (!pending.raised) {
                    const logged = next.$retryLog && next.$retryLog.acquisitionDelay;
                    const delayMs = typeof logged === "number" && logged > 0 ? logged : next.getRetryDelay();
                    raiseRetry(errorInfo, next.getRetryCount() + 1, budget, delayMs);
                }
                return next;
            },
            recordSuccess(token) {
                return inner.recordSuccess(token);
            },
        };
    }

    // Hook computeNextBackoffDelay on a smithy backoff strategy (SDK
    // 3.1174.0: StandardRetryStrategy.retryBackoffStrategy), once per object,
    // so the wrapper above learns the backoff before the strategy sleeps it.
    // The hook returns the computed value unchanged and calls the current
    // observer, if any; an observer failure never affects the retry. Returns
    // the per-object state {observer}, or null when the strategy has no such
    // member (the fallback path).
    function observeBackoff(strategy) {
        if (!strategy || typeof strategy.computeNextBackoffDelay !== "function") {
            return null;
        }
        if (strategy[BACKOFF_STATE] === undefined) {
            const state = { observer: null };
            const compute = strategy.computeNextBackoffDelay;
            strategy.computeNextBackoffDelay = function computeNextBackoffDelay(retryCount) {
                const computed = compute.call(this, retryCount);
                const observer = state.observer;
                if (observer !== null) {
                    try {
                        observer(retryCount, computed);
                    } catch {
                        // never break the retry.
                    }
                }
                return computed;
            };
            Object.defineProperty(strategy, BACKOFF_STATE, { value: state });
        }
        return strategy[BACKOFF_STATE];
    }

    // Route a new client's retry strategy through observingRetryStrategy. The
    // SDK resolves `config.retryStrategy` to a provider function, which its
    // retry middleware calls at the start of every send; replacing the
    // provider on the same config object is therefore seen by every later
    // send. The inner strategy is the one the SDK built from maxAttempts and
    // retryMode (or a caller's clientConfig.retryStrategy).
    function observeRetries(c) {
        const config = c.config;
        const provide = config.retryStrategy;
        if (typeof provide !== "function") {
            return; // not the shape this SDK version resolves; no events.
        }
        const maxAttempts = typeof config.maxAttempts === "function"
            ? config.maxAttempts : async () => 0;
        let wrapped = null;
        config.retryStrategy = () => {
            wrapped ??= Promise.resolve(provide())
                .then((inner) => observingRetryStrategy(inner, maxAttempts));
            return wrapped;
        };
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
        observeRetries(client);
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
        const label = url.hostname.split(".")[0];
        if (!hostSet.has(url.hostname) || (label !== "dynamodb" && label !== "dynamodb-fips")) {
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
            const ctx = { op, holder };
            inFlight.push(ctx);
            try {
                await c.send(cmd);
            } catch (e) {
                if (holder.raw === undefined) {
                    throw new Error(e && e.message ? e.message : String(e));
                }
            } finally {
                inFlight.splice(inFlight.indexOf(ctx), 1);
                notifyRetryListeners({ type: "settled" });
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

        /**
         * Register a UI callback for retry events (P12, D-P12-18). It receives
         *   {type: "retry", operation, attempt, maxAttempts, errorCode,
         *    throttled, delayMs}
         * once for every retry the SDK has decided to make, with the meanings
         * of ddbsh::RetryEvent (attempt 2 = the first retry; maxAttempts 0 =
         * unknown; errorCode "" = no HTTP response; delayMs = the backoff
         * before that attempt), and {type: "settled"} when a send's SDK call
         * resolves or rejects. With the vendored SDK's standard strategy the
         * retry event is raised before the SDK sleeps `delayMs`; with a
         * strategy the bridge cannot hook it may follow the sleep (see
         * observingRetryStrategy). Never raised for a non-retryable error or
         * after the last attempt. A callback that throws is ignored.
         */
        onRetryEvent(cb) {
            if (typeof cb === "function") {
                retryListeners.push(cb);
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
