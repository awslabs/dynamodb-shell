// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// app-core.test.mjs — Node unit tests for the DOM-free P6e UI logic (D-P6e-3).
//
// Zero dependencies (plain asserts + an exit code, like web/sdk-bridge.test.mjs), no
// DOM, no WASM: run directly with `node web/app-core.test.mjs`, or via
// devtools/run-web-smoke alongside the engine smoke. Covers the genuinely new
// P6e logic — the history ring, the prompt builder, the transcript-entry model,
// the file->submission handoff, and the single-flight run guard — plus the P9
// additions: ordered RunResult segments, short-term credential parsing and
// validation (D-P9-3), and the index.html CSP drift check (D-P9-02-9); and the
// P10 additions: history clear() (D-P10-6), the framed / opened / insecure
// credential refusal (G1), the Trusted Types directives in the CSP, the
// cross-tab history removal (historyRemovedElsewhere), the credentials
// controls shipping disabled in index.html, and the header's engine version
// (versionLabel, D-P10-14), busy indicator (createBusyClock, D-P10-16) and
// Help link (D-P10-17); and the P12 additions: the streaming transcript
// (promptEntry, createTranscriptAppender, guardedEventSink; D-P12-17), the
// throttling-code list (isThrottlingError, D-P12-18) and the busy indicator's
// throttled / retrying states (setRetrying, D-P12-19), the HTML result
// table's column growth (createTableModel and the appender's rows/end
// handling, D-P12-25), and the output dropdowns (outputOptions, D-P12-26);
// and the P14 "experimental software" notice (createCredentialNotice and the
// index.html / app.js wiring, B9; D-P14-2..4).
// streamSubmission and the engine's rows events are tested against the built
// engine in streaming.test.mjs.

import { readFileSync } from "node:fs";

import { ALLOWED_HOSTS } from "./vendor/aws-sdk/allowed-hosts.mjs";
import {
    buildPrompt,
    buildTranscriptEntries,
    busyFinishedText,
    busyRetryAnnouncement,
    busyText,
    createBusyClock,
    createCredentialNotice,
    createTableModel,
    createTranscriptAppender,
    guardedEventSink,
    JSON_FLAVOR_CHOICES,
    jsonFlavorApplies,
    OUTPUT_CHOICES,
    outputOptions,
    isThrottlingError,
    promptEntry,
    THROTTLING_ERROR_CODES,
    credentialEnvironmentProblem,
    credentialStatus,
    elapsedSeconds,
    maskAccessKeyId,
    parseCredentials,
    validateCredentials,
    fileToSubmission,
    createHistory,
    createHistoryRecall,
    createRunGuard,
    historyRemovedElsewhere,
    versionLabel,
} from "./app-core.mjs";

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

// ---- buildPrompt -----------------------------------------------------------
console.log("buildPrompt");
eq(buildPrompt("us-east-1", false), "us-east-1> ", "plain region prompt");
eq(buildPrompt("us-west-2", true), "us-west-2(*)> ", "endpoint-overridden marker");
eq(buildPrompt("eu-west-1", false, true), "eu-west-1> ",
   "continuation yields the same prompt (no distinct continuation prompt)");

// ---- buildTranscriptEntries ------------------------------------------------
console.log("buildTranscriptEntries");
eq(buildTranscriptEntries("us-east-1> ", "select * from t", { stdout: "{id: 1}\n", stderr: "" }),
   [{ text: "us-east-1> select * from t", kind: "prompt" },
    { text: "{id: 1}\n", kind: "stdout" }],
   "prompt + stdout, empty stderr dropped");

eq(buildTranscriptEntries("us-east-1> ", "select * from bogus where id = 1",
                          { stdout: "", stderr: "bogus not found\n" }),
   [{ text: "us-east-1> select * from bogus where id = 1", kind: "prompt" },
    { text: "bogus not found\n", kind: "stderr" }],
   "prompt + stderr, empty stdout dropped");

eq(buildTranscriptEntries("r> ", "insert into t (id, v)\nvalues (1, 2)",
                          { stdout: "INSERT\n", stderr: "" }),
   [{ text: "r> insert into t (id, v)\nr> values (1, 2)", kind: "prompt" },
    { text: "INSERT\n", kind: "stdout" }],
   "multi-line input: every line gets the prompt prefix");

eq(buildTranscriptEntries("r> ", "", { stdout: "", stderr: "" }),
   [], "blank submission produces no blocks");

eq(buildTranscriptEntries("r> ", "x", {}),
   [{ text: "r> x", kind: "prompt" }],
   "missing stdout/stderr fields tolerated (prompt only)");

// ---- fileToSubmission ------------------------------------------------------
console.log("fileToSubmission");
eq(fileToSubmission("create table t ( id number ) primary key (id hash);\n"),
   "create table t ( id number ) primary key (id hash);",
   "trailing newline trimmed");
eq(fileToSubmission("a;\nb;\n\n\n"), "a;\nb;", "all trailing newlines trimmed, interior kept");
eq(fileToSubmission("no-newline"), "no-newline", "no trailing newline untouched");
eq(fileToSubmission(undefined), "", "non-string yields empty");

// ---- createHistory ---------------------------------------------------------
console.log("createHistory");
{
    const h = createHistory();
    check(h.size() === 0, "empty history size 0");
    check(h.previous() === null, "previous() on empty history is null");
    check(h.next() === null, "next() on empty history is null");
}
{
    const h = createHistory();
    h.push("one");
    h.push("two");
    h.push("three");
    eq(h.list(), ["one", "two", "three"], "push appends in order");
    check(h.size() === 3, "size reflects pushes");
}
{
    const h = createHistory();
    h.push("same");
    const added = h.push("same");
    check(added === false, "adjacent duplicate is not added");
    eq(h.list(), ["same"], "adjacent duplicate de-duped");
    h.push("other");
    h.push("same");
    eq(h.list(), ["same", "other", "same"], "non-adjacent duplicate is kept");
}
{
    const h = createHistory();
    check(h.push("") === false, "empty string not stored");
    check(h.push("   ") === false, "whitespace-only not stored");
    eq(h.list(), [], "blank entries dropped");
}
{
    const h = createHistory({ limit: 2 });
    h.push("a"); h.push("b"); h.push("c");
    eq(h.list(), ["b", "c"], "ring caps at limit, oldest dropped");
}
{
    const h = createHistory({ entries: ["a", "b", "c", ""], limit: 500 });
    eq(h.list(), ["a", "b", "c"], "seeded entries loaded, blanks filtered");
    // Recall: ArrowUp walks older, clamps at oldest.
    eq(h.previous(), "c", "previous() -> newest first");
    eq(h.previous(), "b", "previous() -> older");
    eq(h.previous(), "a", "previous() -> oldest");
    eq(h.previous(), "a", "previous() clamps at oldest");
    // ArrowDown walks newer, null past the newest (the draft).
    eq(h.next(), "b", "next() -> newer");
    eq(h.next(), "c", "next() -> newest");
    check(h.next() === null, "next() past newest -> null (live draft)");
}
{
    const h = createHistory({ entries: ["a", "b"] });
    h.previous(); // -> "b"
    h.push("c");  // push resets the cursor to the live draft
    check(h.next() === null, "push resets recall cursor to draft");
    eq(h.previous(), "c", "after reset, previous() recalls the newest");
}
{
    // clear() (the page's "Clear history", D-P10-6) empties the ring and the
    // cursor; recording starts again with the next push.
    const h = createHistory({ entries: ["a", "b", "c"] });
    h.previous(); // mid-recall
    h.clear();
    eq(h.list(), [], "clear() empties the ring");
    check(h.size() === 0, "size 0 after clear()");
    check(h.previous() === null && h.next() === null, "no recall after clear()");
    h.push("d");
    eq(h.list(), ["d"], "push after clear() starts a new history");
    eq(h.previous(), "d", "recall works again after clear()");
}
{
    // The page clears the ring and resets recall together; a recall in
    // progress must not resurrect a cleared entry or wipe the live draft.
    const h = createHistory({ entries: ["a", "b"] });
    const r = createHistoryRecall(h);
    eq(r.up("draft"), "b", "recalling before clear()");
    h.clear();
    r.reset();
    check(r.recalling === false, "reset() after clear() ends the recall");
    check(r.up("typed") === null, "up() after clear() is a no-op (nothing to recall)");
    check(r.down() === null, "down() after clear() is a no-op");
}

// ---- historyRemovedElsewhere (P10) -----------------------------------------
// The page's storage-event handler: another tab's "Clear history" must clear
// this tab's in-memory copy too, or this tab's next save writes it back.
console.log("historyRemovedElsewhere");
{
    const KEY = "ddbsh.history";
    check(historyRemovedElsewhere({ key: KEY, newValue: null }, KEY) === true,
        "the history key removed in another tab (Clear history): removed");
    check(historyRemovedElsewhere({ key: null, newValue: null }, KEY) === true,
        "the whole storage area cleared in another tab (key null): removed");
    check(historyRemovedElsewhere({ key: KEY, newValue: JSON.stringify(["show tables;"]) }, KEY) === false,
        "another tab saved its history (a new value): not removed");
    check(historyRemovedElsewhere({ key: KEY, newValue: "" }, KEY) === false,
        "an empty-string value is a value, not a removal");
    check(historyRemovedElsewhere({ key: "other.key", newValue: null }, KEY) === false,
        "another key removed: not removed");
    check(historyRemovedElsewhere({}, KEY) === false, "an event without a key: not removed");
    check(historyRemovedElsewhere(undefined, KEY) === false, "no event at all: not removed");
    check(historyRemovedElsewhere({ key: undefined, newValue: null }, undefined) === false,
        "no history key given: not removed");
    // The handler's whole effect, on the modules app.js uses: this tab's ring
    // and recall are emptied, so its next command saves only itself.
    const h = createHistory({ entries: ["cmd1", "cmd2"] });
    const r = createHistoryRecall(h);
    eq(r.up(""), "cmd2", "this tab is recalling an entry when the event arrives");
    if (historyRemovedElsewhere({ key: KEY, newValue: null }, KEY)) {
        h.clear();
        r.reset();
    }
    check(r.up("") === null, "after the event, nothing is recalled");
    h.push("cmd3");
    eq(h.list(), ["cmd3"], "after the event, this tab saves only its new command");
}

// ---- createHistoryRecall ---------------------------------------------------
console.log("createHistoryRecall");
{
    // Must-fix #1 regression: ArrowDown while NOT recalling must be a no-op and
    // must never wipe the live buffer.
    const r = createHistoryRecall(createHistory({ entries: ["a", "b"] }));
    check(r.recalling === false, "recall starts idle");
    check(r.down() === null, "down() when not recalling is a no-op (null)");
    check(r.recalling === false, "down() did not start a recall");
}
{
    // Nit regression: ArrowUp on empty history must not stash a draft (nothing
    // older to recall), so a later ArrowDown stays a no-op and cannot wipe input.
    const r = createHistoryRecall(createHistory());
    check(r.up("typed") === null, "up() on empty history is a no-op (null)");
    check(r.recalling === false, "up() on empty history did not stash a draft");
    check(r.down() === null, "down() after no-op up() stays a no-op");
}
{
    // Full recall cycle: up stashes the live draft, walks older; down walks
    // newer and restores the exact draft (even when empty) at the live line.
    const r = createHistoryRecall(createHistory({ entries: ["a", "b", "c"] }));
    eq(r.up("draft"), "c", "up() recalls newest and stashes the draft");
    check(r.recalling === true, "recalling after first up()");
    eq(r.up("ignored"), "b", "second up() does not re-stash the draft");
    eq(r.up("ignored"), "a", "third up() -> oldest");
    eq(r.up("ignored"), "a", "up() clamps at the oldest (repeats, does not go null)");
    check(r.recalling === true, "still recalling at the oldest entry");
    eq(r.down(), "b", "down() walks newer");
    eq(r.down(), "c", "down() -> newest");
    eq(r.down(), "draft", "down() past newest restores the stashed draft");
    check(r.recalling === false, "recall ended after the draft was restored");
    check(r.down() === null, "down() after recall ended is a no-op");
}
{
    // The stashed draft may be the empty string: restoring it yields "" (a real
    // value to set), distinct from null (leave the buffer alone).
    const r = createHistoryRecall(createHistory({ entries: ["only"] }));
    eq(r.up(""), "only", "up() with an empty live buffer recalls the entry");
    eq(r.down(), "", "down() restores the empty draft as \"\", not null");
    check(r.recalling === false, "recall ended");
}
{
    // reset() abandons an in-progress recall (manual edit / after submit).
    const r = createHistoryRecall(createHistory({ entries: ["a", "b"] }));
    r.up("draft");
    check(r.recalling === true, "recalling before reset");
    r.reset();
    check(r.recalling === false, "reset() ends the recall");
    eq(r.up("new-draft"), "b", "after reset, up() recalls the newest again");
}

// ---- createRunGuard --------------------------------------------------------
console.log("createRunGuard");
{
    const g = createRunGuard();
    check(g.busy === false, "guard starts idle");
    check(g.begin() === true, "begin() claims the slot");
    check(g.busy === true, "guard busy after begin");
    check(g.begin() === false, "second begin() refused while busy");
    g.end();
    check(g.busy === false, "guard idle after end");
    check(g.begin() === true, "begin() works again after end");
    g.end();
}

// ---- buildTranscriptEntries with ordered segments (P9, D-P9-02-11) ---------
console.log("buildTranscriptEntries: segments");
eq(buildTranscriptEntries("r> ", "x", {
    stdout: "A\nB\n", stderr: "E\n",
    segments: [{ stream: "stdout", text: "A\n" }, { stream: "stderr", text: "E\n" },
        { stream: "stdout", text: "B\n" }],
}), [{ text: "r> x", kind: "prompt" }, { text: "A\n", kind: "stdout" },
    { text: "E\n", kind: "stderr" }, { text: "B\n", kind: "stdout" }],
"segments render in emission order (stderr between two results)");
eq(buildTranscriptEntries("r> ", "x", {
    segments: [{ stream: "stdout", text: "A\n" }, { stream: "stdout", text: "B\n" },
        { stream: "stderr", text: "" }],
}), [{ text: "r> x", kind: "prompt" }, { text: "A\nB\n", kind: "stdout" }],
"adjacent same-stream segments coalesce; empty segments dropped");
{
    const handle = { size: () => 1, get: () => ({ stream: "stderr", text: "E\n" }) };
    eq(buildTranscriptEntries("r> ", "", { segments: handle }), [{ text: "E\n", kind: "stderr" }],
        "an embind vector-like segments handle is accepted");
}

// ---- credentials (P9, D-P9-3 / D-P9-4) -------------------------------------
console.log("credentials");
{
    // Test fixtures. Every value below is fictitious: none was ever issued by
    // AWS and none was ever a valid credential. The key ids are a keyboard row
    // (QWERTYUIOPASDFGH) behind the ASIA/AKIA/XXXX prefixes, SECRET is the
    // example secret from the AWS documentation, and TOKEN is invented. Every
    // expiry, and the clock (NOW) the tests inject, is in 2025, so a value
    // mistaken for a real one would be long expired.
    const NOW = Date.parse("2025-01-15T12:00:00Z");
    const KEY = "ASIAQWERTYUIOPASDFGH";           // short-term shape (accepted)
    const LONG_TERM_KEY = "AKIAQWERTYUIOPASDFGH"; // long-term shape (refused)
    const MALFORMED_KEY = "XXXXQWERTYUIOPASDFGH"; // neither prefix (refused)
    const SECRET = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    const TOKEN = "IQoJb3JpZ2luX2VjEXAMPLETOKEN==";
    const EXP = "2025-01-15T13:00:00Z";     // one hour after NOW
    const EXPIRED = "2025-01-15T11:00:00Z"; // one hour before NOW

    const sts = JSON.stringify({ Credentials: { AccessKeyId: KEY, SecretAccessKey: SECRET,
        SessionToken: TOKEN, Expiration: EXP }, AssumedRoleUser: { Arn: "arn:x" } });
    let r = parseCredentials(sts, NOW);
    check(r.ok && r.credentials.accessKeyId === KEY && r.credentials.sessionToken === TOKEN &&
        r.credentials.expiration.toISOString() === "2025-01-15T13:00:00.000Z", "STS JSON accepted");

    r = parseCredentials(JSON.stringify({ Version: 1, AccessKeyId: KEY, SecretAccessKey: SECRET,
        SessionToken: TOKEN, Expiration: EXP }), NOW);
    check(r.ok && r.credentials.secretAccessKey === SECRET, "credential-process JSON accepted");

    r = parseCredentials(`export AWS_ACCESS_KEY_ID=${KEY}\nexport AWS_SECRET_ACCESS_KEY=${SECRET}\n` +
        `export AWS_SESSION_TOKEN=${TOKEN}\nexport AWS_CREDENTIAL_EXPIRATION=${EXP}\n`, NOW);
    check(r.ok && r.credentials.expiration instanceof Date, "bash export lines accepted");

    r = parseCredentials(`AWS_ACCESS_KEY_ID="${KEY}"\r\nAWS_SECRET_ACCESS_KEY='${SECRET}'\r\n` +
        `AWS_SESSION_TOKEN=${TOKEN}\r\n# comment\r\nAWS_REGION=us-east-1\r\n`, NOW);
    check(r.ok && r.credentials.accessKeyId === KEY && r.credentials.secretAccessKey === SECRET &&
        r.credentials.expiration === undefined, "env-no-export lines, quotes, CRLF, unknown keys ignored");

    r = parseCredentials(`$Env:AWS_ACCESS_KEY_ID="${KEY}"\n$Env:AWS_SECRET_ACCESS_KEY="${SECRET}"\n` +
        `$Env:AWS_SESSION_TOKEN="${TOKEN}"`, NOW);
    check(r.ok, "PowerShell $Env: lines accepted");

    r = parseCredentials(`set AWS_ACCESS_KEY_ID=${KEY}\nset AWS_SECRET_ACCESS_KEY=${SECRET}\n` +
        `set AWS_SESSION_TOKEN=${TOKEN}`, NOW);
    check(r.ok, "cmd set lines accepted");

    const refused = [
        [JSON.stringify({ AccessKeyId: LONG_TERM_KEY, SecretAccessKey: SECRET }), /long-term access keys \(AKIA…\)/],
        [`AWS_ACCESS_KEY_ID=${LONG_TERM_KEY}\nAWS_SECRET_ACCESS_KEY=${SECRET}\nAWS_SESSION_TOKEN=${TOKEN}`, /long-term/],
        [`AWS_ACCESS_KEY_ID=${KEY}\nAWS_SECRET_ACCESS_KEY=${SECRET}`, /a session token is required/],
        [`AWS_ACCESS_KEY_ID=${MALFORMED_KEY}\nAWS_SECRET_ACCESS_KEY=${SECRET}\nAWS_SESSION_TOKEN=${TOKEN}`, /not an AWS short-term access key id/],
        [`AWS_ACCESS_KEY_ID=${KEY}\nAWS_SESSION_TOKEN=${TOKEN}`, /secret access key is missing/],
        [`AWS_ACCESS_KEY_ID=${KEY}\nAWS_SECRET_ACCESS_KEY=a b\nAWS_SESSION_TOKEN=${TOKEN}`, /secret access key is missing or malformed/],
        [JSON.stringify({ Credentials: { AccessKeyId: KEY, SecretAccessKey: SECRET, SessionToken: TOKEN,
            Expiration: EXPIRED } }), /these credentials expired at 2025-01-15T11:00:00.000Z/],
        [JSON.stringify({ Credentials: { AccessKeyId: KEY, SecretAccessKey: SECRET, SessionToken: TOKEN,
            Expiration: "soon" } }), /not a valid date/],
        ["{not json", /does not parse/],
        ["hello world", /no AWS credentials found/],
        ["   ", /no credentials were pasted/],
    ];
    for (const [text, re] of refused) {
        const out = parseCredentials(text, NOW);
        check(!out.ok && re.test(out.message), `refused: ${out.message}`);
        check(out.ok || (!out.message.includes(SECRET) && !out.message.includes(TOKEN)),
            "  refusal message reveals no secret or token");
    }

    check(validateCredentials({ accessKeyId: KEY, secretAccessKey: SECRET, sessionToken: TOKEN }, NOW) === null,
        "validateCredentials accepts a complete set without expiration");
    eq(maskAccessKeyId(KEY), "ASIA…DFGH", "maskAccessKeyId shows prefix + last 4");
    eq(credentialStatus(null, NOW), "none", "status: none");
    eq(credentialStatus({ accessKeyId: KEY, expiration: new Date(EXP) }, NOW),
        "ASIA…DFGH, expires 13:00Z (in 60 min)", "status: expires");
    eq(credentialStatus({ accessKeyId: KEY, expiration: new Date(EXP) }, Date.parse(EXP) + 1), "expired",
        "status: expired");
    eq(credentialStatus({ accessKeyId: KEY }, NOW), "ASIA…DFGH (expiry unknown)", "status: expiry unknown");
}

// ---- where credentials may be used (P10, G1) -------------------------------
console.log("credentialEnvironmentProblem");
{
    const fine = { secureContext: true, framed: false, opened: false };
    check(credentialEnvironmentProblem(fine) === null, "secure, top-level, not opened: accepted");
    const framed = credentialEnvironmentProblem({ ...fine, framed: true });
    check(/refused/.test(framed) && /inside a frame/.test(framed) && /open the page directly in its own tab/.test(framed),
        `framed: refused — ${framed}`);
    const opened = credentialEnvironmentProblem({ ...fine, opened: true });
    check(/refused/.test(opened) && /opened this window/.test(opened) && /open the page directly/.test(opened),
        `opened by another page's script: refused — ${opened}`);
    const insecure = credentialEnvironmentProblem({ ...fine, secureContext: false });
    check(/https:\/\/ or http:\/\/localhost/.test(insecure), `not a secure context: refused — ${insecure}`);
    eq(credentialEnvironmentProblem({ secureContext: false, framed: true, opened: true }), framed,
        "framed is reported first when several apply");
    eq(credentialEnvironmentProblem({ secureContext: false, framed: false, opened: true }), opened,
        "opened is reported before the secure-context problem");
    check(credentialEnvironmentProblem({}) !== null, "unknown facts are refused (fail closed)");
    check(credentialEnvironmentProblem() !== null, "no facts at all are refused (fail closed)");
    check(credentialEnvironmentProblem({ secureContext: "yes", framed: 0, opened: "" }) !== null,
        "only the boolean false/true values count (fail closed)");
}

// ---- CSP drift check (D-P9-02-9) -------------------------------------------
console.log("CSP");
{
    const html = readFileSync(new URL("./index.html", import.meta.url), "utf8");
    const m = html.match(/<meta http-equiv="Content-Security-Policy" content="([^"]*)">/);
    check(m !== null, "index.html carries a CSP meta");
    const directives = Object.fromEntries((m ? m[1] : "").split(";").map((d) => d.trim())
        .filter(Boolean).map((d) => { const [k, ...v] = d.split(/\s+/); return [k, v]; }));
    eq(directives["script-src"], ["'self'", "'wasm-unsafe-eval'"], "script-src is exactly 'self' 'wasm-unsafe-eval'");
    eq(directives["connect-src"], ["'self'", ...ALLOWED_HOSTS.map((h) => `https://${h}`)],
        "connect-src is exactly 'self' + ALLOWED_HOSTS");
    eq(directives["default-src"], ["'none'"], "default-src 'none'");
    eq(directives["object-src"], ["'none'"], "object-src 'none'");
    eq(directives["base-uri"], ["'none'"], "base-uri 'none'");
    eq(directives["require-trusted-types-for"], ["'script'"], "require-trusted-types-for 'script' (Trusted Types, D-P10-12)");
    eq(directives["trusted-types"], ["'none'"], "trusted-types 'none' (no Trusted Types policy may be created)");
    check(!("frame-ancestors" in directives), "no frame-ancestors in the <meta> (browsers ignore it there; the host sends it)");
    check(!/unsafe-inline|'unsafe-eval'/.test(m ? m[1] : ""), "no 'unsafe-inline' or 'unsafe-eval'");
    check(!/<script(?![^>]*\bsrc=)[^>]*>/i.test(html), "no inline <script> in index.html");
    check(!/\sstyle=/i.test(html) && !/<style/i.test(html), "no inline style in index.html");
    check(ALLOWED_HOSTS.every((h) => /^(search-)?dynamodb(-fips)?\.[a-z0-9-]+\.(amazonaws\.com|api\.aws)$/.test(h)),
        "every allowed host is [search-]dynamodb[-fips].<region>.{amazonaws.com,api.aws}");
    check(ALLOWED_HOSTS.includes("search-dynamodb.us-east-1.amazonaws.com"),
        "the SearchVectors host is allowed");
}

// ---- controls that ship disabled (P10) -------------------------------------
// The credentials text box and "Use credentials" stay disabled until app.js has
// run and found the environment safe (credentialEnvironmentProblem), so a page
// whose script never runs (scripts blocked, a sandboxed frame, modules served
// with the wrong type) never invites a paste. The editor and Run wait for the
// engine the same way. Forget stays enabled: forgetting is always safe.
console.log("controls that ship disabled");
{
    const html = readFileSync(new URL("./index.html", import.meta.url), "utf8");
    const openingTag = (id) => {
        const m = html.match(new RegExp(`<[a-z]+\\s[^>]*\\bid="${id}"[^>]*>`));
        return m ? m[0] : "";
    };
    const shipsDisabled = (id) => /\sdisabled(?=[\s>=])/.test(openingTag(id));
    for (const id of ["cred-input", "cred-use-btn", "editor", "run-btn", "file-input"]) {
        check(openingTag(id) !== "", `index.html has #${id}`);
        check(shipsDisabled(id), `#${id} ships disabled`);
    }
    check(openingTag("cred-forget-btn") !== "" && !shipsDisabled("cred-forget-btn"),
        "#cred-forget-btn ships enabled");
}

// ---- header: version (P10, D-P10-14) ---------------------------------------
console.log("versionLabel");
eq(versionLabel({ version: () => "0.12.1" }), "version 0.12.1", "the engine's version");
eq(versionLabel({ version: () => "0.13" }), "version 0.13", "a two-part version");
eq(versionLabel({ version: () => "1.2.3.4" }), "version 1.2.3.4", "a four-part version");
eq(versionLabel({}), "", "an older ddbsh.wasm without the export: the title stays 'ddbsh (web)'");
eq(versionLabel(null), "", "no module");
eq(versionLabel({ version: "0.12.1" }), "", "a non-function export is ignored");
eq(versionLabel({ version: () => { throw new Error("boom"); } }), "", "a throwing export is ignored");
eq(versionLabel({ version: () => "" }), "", "an empty version is ignored");
eq(versionLabel({ version: () => "0.12.1 <b>" }), "", "anything but a version is ignored");
eq(versionLabel({ version: () => 12 }), "", "a non-string is ignored");

// ---- header: busy indicator (P10, D-P10-16) --------------------------------
console.log("busy indicator");
eq(elapsedSeconds(1000, 1000), 0, "elapsed: 0 at the start");
eq(elapsedSeconds(1000, 1999.9), 0, "elapsed: whole seconds, rounded down");
eq(elapsedSeconds(1000, 2000), 1, "elapsed: 1 after one second");
eq(elapsedSeconds(1000, 62500), 61, "elapsed: 61 after 61.5 s");
eq(elapsedSeconds(1000, 500), 0, "elapsed: never negative");
eq(busyText(0), "running… 0s", "busy text at the start");
eq(busyText(42), "running… 42s", "busy text after 42 s");
eq(busyFinishedText(0), "command finished after 0 seconds", "finished: 0 seconds");
eq(busyFinishedText(1), "command finished after 1 second", "finished: 1 second (singular)");
eq(busyFinishedText(5), "command finished after 5 seconds", "finished: 5 seconds");
{
    // A fake monotonic clock and animation-frame queue: frames(ms) runs one
    // frame every 16 ms of fake time, in request order, like a 60 Hz display.
    let nowMs = 5000;
    let nextHandle = 1;
    let queued = []; // [{ handle, fn }]
    let requests = 0;
    const renders = [];
    const announcements = [];
    const io = {
        now: () => nowMs,
        requestFrame: (fn) => {
            requests += 1;
            const handle = nextHandle++;
            queued.push({ handle, fn });
            return handle;
        },
        cancelFrame: (handle) => {
            queued = queued.filter((q) => q.handle !== handle);
        },
        render: (text) => renders.push(text),
        announce: (text) => announcements.push(text),
    };
    const frames = (ms, step = 16) => {
        const until = nowMs + ms;
        while (nowMs + step <= until) {
            nowMs += step;
            const run = queued;
            queued = [];
            for (const q of run) {
                q.fn();
            }
        }
        nowMs = until;
    };
    const clock = createBusyClock(io);
    check(!clock.running, "idle before start");
    check(clock.stop() === false, "stop() while idle does nothing");
    eq(renders, [], "  … and renders nothing");
    eq(announcements, [], "  … and announces nothing");

    check(clock.start() === true, "start() starts");
    check(clock.running, "running after start");
    eq(renders, ["running… 0s"], "start shows 0s at once");
    eq(announcements, ["command started"], "start is announced once");
    eq(queued.length, 1, "start requests one animation frame");
    check(clock.start() === false, "a second start() while running does nothing");
    eq(renders.length, 1, "  … and does not re-render");
    eq(queued.length, 1, "  … and requests no second frame");

    frames(3500);
    eq(renders, ["running… 0s", "running… 1s", "running… 2s", "running… 3s"],
        "one render per whole second over 218 frames (3.5 s elapsed); frames in between render nothing");
    eq(announcements, ["command started"], "the seconds are not announced");
    eq(queued.length, 1, "exactly one frame pending while running");

    nowMs = 5000 + 4900;
    check(clock.stop() === true, "stop() stops");
    check(!clock.running, "idle after stop");
    eq(renders[renders.length - 1], null, "stop hides the indicator");
    eq(announcements, ["command started", "command finished after 4 seconds"],
        "stop announces the elapsed whole seconds");
    eq(queued, [], "stop cancels the pending frame");
    const rendersAtStop = renders.length;
    const requestsAtStop = requests;
    frames(5000);
    eq(renders.length, rendersAtStop, "no render after stop");
    eq(requests, requestsAtStop, "no frame requested after stop");

    // A quick submission: start and stop in the same task.
    check(clock.start() === true && clock.stop() === true, "start then stop at once");
    eq(renders.slice(rendersAtStop), ["running… 0s", null], "shown then hidden");
    eq(announcements.slice(2), ["command started", "command finished after 0 seconds"],
        "both announced");
    eq(queued, [], "no frame left pending");

    // A frame callback that runs after stop() (already dequeued) does nothing.
    const stale = [];
    const staleClock = createBusyClock({
        ...io,
        requestFrame: (fn) => { stale.push(fn); return 99; },
        cancelFrame: () => {},
        render: (text) => stale.push(text),
    });
    staleClock.start();
    const queuedFrame = stale[1];
    staleClock.stop();
    const before = stale.length;
    queuedFrame();
    eq(stale.length, before, "a frame that runs after stop() renders and requests nothing");

    // No frames for a while (a hidden tab, or a blocked main thread): the next
    // frame shows the true elapsed seconds.
    renders.length = 0;
    clock.start();
    frames(1100);
    nowMs += 2000; // 2 s without frames
    frames(16);
    eq(renders, ["running… 0s", "running… 1s", "running… 3s"],
        "after 2 s without frames the next frame shows 3s (the true elapsed time)");
    clock.stop();
}

// ---- busy indicator: throttled / retrying (P12, D-P12-19) -------------------
console.log("busy indicator: retry states");
eq(busyText(12, "running"), "running… 12s", "running state text");
eq(busyText(12, "throttled"), "running… 12s · throttled, retrying", "throttled state text");
eq(busyText(12, "retrying"), "running… 12s · retrying", "retrying state text");
eq(busyRetryAnnouncement("throttled"), "DynamoDB is throttling; retrying", "throttled announcement");
eq(busyRetryAnnouncement("retrying"), "retrying", "retrying announcement");
{
    let nowMs = 1000;
    const renders = [];
    const announcements = [];
    let pending = null;
    const clock = createBusyClock({
        now: () => nowMs,
        requestFrame: (fn) => {
            pending = fn;
            return 1;
        },
        cancelFrame: () => {
            pending = null;
        },
        render: (text, state) => renders.push([text, state]),
        announce: (text) => announcements.push(text),
    });
    const frame = () => {
        const fn = pending;
        pending = null;
        if (fn) {
            fn();
        }
    };
    check(clock.setRetrying("throttled") === false, "setRetrying while idle is ignored");
    eq([renders, announcements, clock.state], [[], [], null], "  … nothing rendered or announced; state idle");

    clock.start();
    eq(clock.state, "running", "start enters running");
    eq(renders, [["running… 0s", "running"]], "running renders its text and state");
    nowMs = 3500;
    check(clock.setRetrying("throttled") === true, "a throttled retry enters throttled");
    eq(clock.state, "throttled", "  state throttled");
    eq(renders[renders.length - 1], ["running… 2s · throttled, retrying", "throttled"],
        "  re-renders at once with the elapsed seconds and the state");
    eq(announcements, ["command started", "DynamoDB is throttling; retrying"], "  announced once");
    const count = renders.length;
    clock.setRetrying("throttled");
    eq(renders.length, count, "the same state again renders nothing");
    nowMs = 4100;
    frame();
    eq(renders[renders.length - 1], ["running… 3s · throttled, retrying", "throttled"],
        "the seconds keep counting in the throttled state");
    check(clock.setRetrying(null) === true, "settled returns to running");
    eq(renders[renders.length - 1], ["running… 3s", "running"], "  running text and state");
    clock.setRetrying("throttled");
    clock.setRetrying(null);
    clock.setRetrying("retrying");
    eq(renders[renders.length - 1], ["running… 3s · retrying", "retrying"], "a non-throttle retry: retrying");
    eq(announcements, ["command started", "DynamoDB is throttling; retrying"],
        "one retry announcement per submission, however many retries");
    check(clock.setRetrying("bogus") === false, "an unknown kind is refused");
    eq(clock.state, "retrying", "  and changes nothing");
    clock.stop();
    eq([renders[renders.length - 1], clock.state], [[null, null], null], "stop hides the indicator from any state");
    check(clock.setRetrying(null) === false, "settled after stop is ignored");

    // The next submission starts in running, with the announcement re-armed.
    renders.length = 0;
    announcements.length = 0;
    clock.start();
    eq(renders, [["running… 0s", "running"]], "the next submission starts in running");
    clock.setRetrying("retrying");
    clock.setRetrying("throttled");
    eq(announcements, ["command started", "retrying"],
        "the next submission announces its first retry state (once)");
    clock.stop();
}
{
    // The pulse is off under prefers-reduced-motion; the state colors stay.
    const css = readFileSync(new URL("./styles.css", import.meta.url), "utf8");
    check(/\.busy\.busy-throttled\s*\{[^}]*animation:\s*ddbsh-pulse/.test(css),
        "styles.css: the throttled state pulses");
    check(/\.busy\.busy-throttled,\s*\.busy\.busy-retrying\s*\{[^}]*color:/.test(css),
        "styles.css: the throttled and retrying states have their own color");
    const reduced = css.match(/@media \(prefers-reduced-motion: reduce\)\s*\{([\s\S]*?)\n\}/g) || [];
    check(reduced.some((block) => /\.busy\.busy-throttled\s*\{\s*animation:\s*none;/.test(block)),
        "styles.css: no pulse under prefers-reduced-motion: reduce");
}

// ---- streaming transcript (P12, D-P12-17) ------------------------------------
console.log("streaming transcript");
eq(promptEntry("p> ", "a;\nb;"), { text: "p> a;\np> b;", kind: "prompt" }, "promptEntry prefixes every line");
eq(promptEntry("p> ", ""), null, "promptEntry: nothing for an empty input");
{
    const blocks = [];
    const appender = createTranscriptAppender({
        createBlock: (kind) => {
            const b = { kind, parts: [], attached: true };
            blocks.push(b);
            return b;
        },
        appendText: (b, t) => b.parts.push(t),
        isAttached: (b) => b.attached,
    });
    const view = () => blocks.map((b) => [b.kind, b.parts]);
    appender.prompt("p> ", "show tables;");
    appender.event({ type: "text", stream: "stdout", text: "t1\n" });
    appender.event({ type: "text", stream: "stdout", text: "t2\n" });
    eq(view(), [["prompt", ["p> show tables;"]], ["stdout", ["t1\n", "t2\n"]]],
        "the prompt opens a block; adjacent stdout events share one block, one part each");
    appender.event({ type: "text", stream: "stderr", text: "oops\n" });
    appender.event({ type: "text", stream: "stdout", text: "t3\n" });
    eq(view().slice(2), [["stderr", ["oops\n"]], ["stdout", ["t3\n"]]], "a change of stream starts a new block");
    appender.event({ type: "text", stream: "stdout", text: "" });
    appender.event({ type: "rows", rows: [] });
    appender.event(null);
    eq(blocks.length, 4, "empty text, a non-text event and null add nothing");
    blocks[3].attached = false; // the user pressed Clear mid-run.
    appender.write("stdout", "t4\n");
    eq(view().slice(4), [["stdout", ["t4\n"]]], "after Clear the next text starts a new block");
    appender.prompt("p> ", "next;");
    appender.write("prompt", "unused");
    eq(blocks[5].kind, "prompt", "prompt() always opens its own block");
    // The result matches buildTranscriptEntries' model for the same segments.
    const blocks2 = [];
    const a2 = createTranscriptAppender({
        createBlock: (kind) => {
            const b = { kind, text: "" };
            blocks2.push(b);
            return b;
        },
        appendText: (b, t) => {
            b.text += t;
        },
        isAttached: () => true,
    });
    const segments = [{ stream: "stdout", text: "A\n" }, { stream: "stdout", text: "B\n" },
        { stream: "stderr", text: "E\n" }, { stream: "stdout", text: "C\n" }];
    a2.prompt("p> ", "x;\ny;");
    for (const seg of segments) {
        a2.event({ type: "text", ...seg });
    }
    eq(blocks2.map((b) => ({ text: b.text, kind: b.kind })),
        buildTranscriptEntries("p> ", "x;\ny;", { segments }),
        "the appender builds the same blocks as buildTranscriptEntries");
}
{
    const seen = [];
    const g = guardedEventSink((ev) => {
        if (ev === 2 || ev === 3) {
            throw new Error(`bad ${ev}`);
        }
        seen.push(ev);
    });
    eq(g.error, null, "guardedEventSink: no error before any failure");
    for (const ev of [1, 2, 3, 4]) {
        g.emit(ev); // never throws
    }
    eq(seen, [1, 4], "events after a failure are still delivered");
    eq(g.error.message, "bad 2", "the first exception is kept");
    const g2 = guardedEventSink(() => {
        throw undefined; // eslint-disable-line no-throw-literal
    });
    g2.emit({});
    check(g2.error === undefined, "a thrown undefined is kept as thrown (not mistaken for none)");
    eq(guardedEventSink(() => {}).error, null, "a callback that never throws leaves error null");
}

// ---- throttling codes (P12, D-P12-18) -----------------------------------------
console.log("throttling codes");
// The same six codes as ddbsh::is_throttling_error (src/retry_event.cpp);
// tests/wire_client_retry_test.cpp pins the C++ copy. Change both together.
eq([...THROTTLING_ERROR_CODES], ["ThrottlingException", "ProvisionedThroughputExceededException",
    "RequestLimitExceeded", "TooManyRequestsException", "LimitExceededException",
    "TransactionInProgressException"], "the throttling list");
check(Object.isFrozen(THROTTLING_ERROR_CODES), "the list is frozen");
check(THROTTLING_ERROR_CODES.every(isThrottlingError), "every listed code is throttling");
for (const code of ["ResourceNotFoundException", "InternalServerError", "ConditionalCheckFailedException",
    "", "throttlingexception", "com.amazonaws.dynamodb.v20120810#ThrottlingException", "THROTTLING"]) {
    check(!isThrottlingError(code), `not throttling: ${JSON.stringify(code)}`);
}
check(!isThrottlingError(undefined) && !isThrottlingError(null), "a missing code is not throttling");

// ---- header: index.html markup (P10, D-P10-14 / D-P10-16 / D-P10-17) -------
console.log("header markup");
{
    const html = readFileSync(new URL("./index.html", import.meta.url), "utf8");
    const openingTag = (id) => {
        const m = html.match(new RegExp(`<[a-z]+\\s[^>]*\\bid="${id}"[^>]*>`));
        return m ? m[0] : "";
    };
    const attr = (tag, name) => {
        const m = tag.match(new RegExp(`\\s${name}="([^"]*)"`));
        return m ? m[1] : null;
    };
    eq(html.match(/<title>([^<]*)<\/title>/)?.[1], "ddbsh (web)", "the tab title stays 'ddbsh (web)'");
    check(/<span id="version-label" class="muted"><\/span>/.test(html),
        "#version-label ships empty (app.js fills it from the engine)");
    const help = openingTag("help-link");
    check(help.startsWith("<a "), "#help-link is a link");
    eq(attr(help, "href"),
        "https://github.com/awslabs/dynamodb-shell/blob/main/docs/configurations/03-browser-aws.md",
        "#help-link opens the browser guide on main");
    eq(attr(help, "target"), "_blank",
        "#help-link opens a new tab (leaving the page would forget the credentials)");
    eq((attr(help, "rel") || "").split(/\s+/).sort(), ["noopener", "noreferrer"],
        "#help-link has rel=\"noopener noreferrer\"");
    const status = openingTag("busy-status");
    eq(attr(status, "role"), "status", "#busy-status is a status region");
    eq(attr(status, "aria-live"), "polite", "#busy-status is a polite live region");
    const busy = openingTag("busy");
    check(busy !== "" && attr(busy, "aria-live") === null && attr(busy, "role") === null,
        "#busy (the ticking text) is not a live region");
}

// ---- HTML result tables (P12, D-P12-25) ---------------------------------------
console.log("createTableModel");
{
    const cell = (t, n = false, m = false) => ({ t, n, m });
    const model = createTableModel();
    const p1 = model.apply({ type: "rows", columns: ["pk", "a"], newColumnsFrom: 0,
        rows: [[cell("1", true), cell("x\ny")]], groupEnd: true });
    eq(p1, { addHeaders: ["pk", "a"], padRows: 0, padCells: 0,
        rows: [[{ text: "1", className: "num" }, { text: "x\ny", className: "" }]] },
        "the first event: every header, its rows; numbers get the num class, text is kept as is");
    const p2 = model.apply({ type: "rows", columns: ["pk", "a"], newColumnsFrom: 2,
        rows: [[cell("2", true), cell("", false, true)]], groupEnd: true });
    eq(p2, { addHeaders: [], padRows: 0, padCells: 0,
        rows: [[{ text: "2", className: "num" }, { text: "", className: "missing" }]] },
        "no new column: no header, no padding; a missing attribute is an empty missing cell");
    const p3 = model.apply({ type: "rows", columns: ["pk", "a", "b", "c"], newColumnsFrom: 2,
        rows: [[cell("3", true), cell("y"), cell("{\"k\": 3}"), cell("", false, true)]], groupEnd: true });
    eq([p3.addHeaders, p3.padRows, p3.padCells], [["b", "c"], 2, 2],
        "two new columns: two headers, and two empty cells for each of the two earlier rows");
    eq(p3.rows[0].map((c) => c.className), ["num", "", "", "missing"], "the new row has a cell per column");
    eq(model.size, { columns: 4, rows: 3 }, "the model counts the table's columns and rows");
    const p4 = model.apply({ type: "rows", columns: ["pk", "a", "b", "c"], newColumnsFrom: 4, rows: [] });
    eq(p4, { addHeaders: [], padRows: 0, padCells: 0, rows: [] }, "an event with nothing new plans nothing");
    // A short row (fewer cells than columns) is padded with missing cells.
    const p5 = model.apply({ type: "rows", columns: ["pk", "a", "b", "c"], rows: [[cell("9", true)]] });
    eq(p5.rows[0].map((c) => c.className), ["num", "missing", "missing", "missing"],
        "a row shorter than the columns is completed with missing cells");
    // The empty-result header: columns, no rows.
    const empty = createTableModel().apply({ type: "rows", columns: ["pk"], newColumnsFrom: 0, rows: [] });
    eq(empty, { addHeaders: ["pk"], padRows: 0, padCells: 0, rows: [] }, "an empty result: the header only");
    // New columns before any row: headers only, nothing to pad.
    const m2 = createTableModel();
    m2.apply({ columns: ["pk"], rows: [] });
    eq(m2.apply({ columns: ["pk", "x"], rows: [] }), { addHeaders: ["x"], padRows: 0, padCells: 0, rows: [] },
        "a new column with no row yet pads nothing");
}

console.log("transcript appender: rows and end");
{
    const blocks = [];
    const plans = [];
    const appender = createTranscriptAppender({
        createBlock: (kind) => {
            const b = { kind, parts: [], attached: true };
            blocks.push(b);
            return b;
        },
        appendText: (b, t) => b.parts.push(t),
        isAttached: (b) => b.attached,
        createTable: () => {
            const b = { kind: "table", plans: [], attached: true };
            blocks.push(b);
            return b;
        },
        applyTablePlan: (b, plan) => {
            b.plans.push(plan);
            plans.push(plan);
        },
    });
    const rows = (columns, n) => ({ type: "rows", columns, newColumnsFrom: 0,
        rows: Array.from({ length: n }, () => columns.map(() => ({ t: "v", n: false, m: false }))), groupEnd: true });
    appender.prompt("p> ", "select …;");
    appender.event(rows(["a"], 1));
    appender.event(rows(["a", "b"], 1));
    appender.event({ type: "end" });
    appender.event({ type: "text", stream: "stdout", text: "SELECT (t, 1, 0, 0)\n" });
    appender.event(rows(["x"], 2));
    eq(blocks.map((b) => b.kind), ["prompt", "table", "stdout", "table"],
        "rows events share a table until end; text after it is a new block; the next set a new table");
    eq(blocks[1].plans.map((p) => [p.addHeaders, p.padRows, p.padCells]), [[["a"], 0, 0], [["b"], 1, 1]],
        "the table's model pads the earlier row when page 2 adds a column");
    eq(blocks[3].plans[0].addHeaders, ["x"], "the second table has its own model");
    blocks[3].attached = false; // Clear while the set is open
    appender.event(rows(["x"], 1));
    eq(blocks.map((b) => b.kind), ["prompt", "table", "stdout", "table", "table"],
        "a rows event after Clear starts a new table");
    eq(blocks[4].plans[0].addHeaders, ["x"], "  … with its headers again");
    // Without the table operations, rows events are ignored (a text-only consumer).
    const textOnly = [];
    const a2 = createTranscriptAppender({
        createBlock: (kind) => {
            const b = { kind, parts: [] };
            textOnly.push(b);
            return b;
        },
        appendText: (b, t) => b.parts.push(t),
        isAttached: () => true,
    });
    a2.event(rows(["a"], 1));
    a2.event({ type: "end" });
    a2.event({ type: "text", stream: "stdout", text: "x\n" });
    eq(textOnly.map((b) => b.kind), ["stdout"], "a text-only appender ignores rows and end");
}

// ---- output dropdowns (P12, D-P12-26) ----------------------------------------
console.log("output dropdowns");
{
    eq(OUTPUT_CHOICES.map(([v]) => v), ["default", "json", "csv", "table", "html"], "the Output values, in order");
    eq(OUTPUT_CHOICES.map(([, l]) => l), ["Default", "JSON", "CSV", "Table (text)", "Table (HTML)"],
        "the Output labels");
    eq(JSON_FLAVOR_CHOICES, [["plain", "Plain"], ["dynamodb", "DynamoDB"]], "the JSON choices");
    eq(OUTPUT_CHOICES.map(([v]) => jsonFlavorApplies(v)), [false, true, true, true, true],
        "JSON is disabled for Default only");
    eq(outputOptions("default", "dynamodb"), { output: "default" }, "Default sends no flavor");
    eq(outputOptions("json", "dynamodb"), { output: "json", jsonFlavor: "dynamodb" }, "JSON + DynamoDB");
    eq(outputOptions("html", "plain"), { output: "html", jsonFlavor: "plain" }, "Table (HTML) + Plain");
    eq(outputOptions("csv", "bogus"), { output: "csv", jsonFlavor: "plain" }, "an unknown flavor falls back to plain");
    eq(outputOptions("xml", "dynamodb"), { output: "default" }, "an unknown output falls back to Default");
    eq(outputOptions(undefined, undefined), { output: "default" }, "no values: Default");

    const html = readFileSync(new URL("./index.html", import.meta.url), "utf8");
    for (const id of ["output-select", "flavor-select"]) {
        check(new RegExp(`<select id="${id}"[^>]*></select>`).test(html), `#${id} is an empty <select> (app.js fills it)`);
        check(new RegExp(`<label[^>]*\\bfor="${id}"[^>]*>`).test(html), `#${id} has a <label for>`);
    }
    check(html.indexOf('id="run-btn"') < html.indexOf('id="output-select"') &&
        html.indexOf('id="output-select"') < html.indexOf('id="flavor-select"'),
        "the dropdowns follow the Run button, Output first");
}

// ---- the "experimental software" notice (P14, B9; D-P14-2..4) -----------------
console.log("credentials notice");
{
    // Trap every storage and cookie surface while the notice runs: nothing may
    // touch them (D-P14-2: no flag, no cookie, no local storage).
    const touched = [];
    const saved = {};
    for (const name of ["localStorage", "sessionStorage", "indexedDB", "document"]) {
        saved[name] = Object.getOwnPropertyDescriptor(globalThis, name);
        Object.defineProperty(globalThis, name, {
            configurable: true,
            get() {
                touched.push(name);
                return undefined;
            },
        });
    }

    let shown = 0;
    const applied = [];
    const n = createCredentialNotice({
        show: () => { shown += 1; },
        apply: (c) => { applied.push(c); return "applied"; },
    });
    const c1 = { accessKeyId: "ASIAAAAAAAAAAAAAAAAA", secretAccessKey: "s1", sessionToken: "t1" };
    const c2 = { accessKeyId: "ASIABBBBBBBBBBBBBBBB", secretAccessKey: "s2", sessionToken: "t2" };
    eq(n.pending, false, "nothing held at first");
    eq(n.accept(), undefined, "accept with nothing held applies nothing");
    n.request(c1);
    eq(shown, 1, "the notice shows on the first submission");
    eq(applied.length, 0, "the credentials are not used before Accept");
    eq(n.pending, true, "the credentials are held until Accept");
    eq(n.accept(), "applied", "accept returns apply's result");
    check(applied.length === 1 && applied[0] === c1, "Accept applies the held credentials");
    eq(n.pending, false, "nothing held after Accept");
    eq(n.accept(), undefined, "a second Accept applies nothing");
    eq(applied.length, 1, "the credentials are applied once");
    n.request(c2);
    eq(shown, 2, "the notice shows again on the next submission (nothing records it was seen)");
    eq(applied.length, 1, "the second credentials are not used before Accept");
    n.request(c1);
    eq(shown, 3, "the notice shows on every submission");
    n.accept();
    check(applied.length === 2 && applied[1] === c1, "a newer submission replaces the held one");
    n.request(c2);
    n.discard();
    eq(n.pending, false, "discard drops the held credentials");
    eq(n.accept(), undefined, "Accept after discard applies nothing");
    eq(applied.length, 2, "discarded credentials are never applied");
    eq(touched, [], "the notice touches no storage and no cookie");

    for (const [name, desc] of Object.entries(saved)) {
        if (desc === undefined) {
            delete globalThis[name];
        } else {
            Object.defineProperty(globalThis, name, desc);
        }
    }

    // The notice markup: the D-P14-4 text, word for word, and one button.
    const html = readFileSync(new URL("./index.html", import.meta.url), "utf8");
    const m = html.match(/<dialog id="notice"[^>]*>([\s\S]*?)<\/dialog>/);
    check(m !== null, "index.html has the <dialog id=\"notice\">");
    const inner = m ? m[1] : "";
    const rendered = inner.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ").trim()
        .replace(/ ([.,])/g, "$1");
    const expected = [
        "dynamodb-shell (ddbsh) is experimental software. Use it at your own risk.",
        "Credentials. Your credentials are kept in this tab's memory only. ddbsh never saves them " +
            "to local storage or anywhere else, and never logs them. It uses them only to sign requests to " +
            "AWS. They are wiped when you close or leave this page.",
        "Command history. ddbsh sets no cookies. It saves the statements you type in the editor " +
            "(the last 500, in full, including any values in them) in your browser's local storage for " +
            "this site, so they are still there the next time you open the page. Statements run from an " +
            "uploaded file are not saved. Each open tab keeps its own history while it is open, and the tab " +
            "that saved last is the one stored. \"Clear history\" deletes the stored history and clears every " +
            "open ddbsh tab.",
        "No warranty. ddbsh is open-source software under the Apache License 2.0. It is provided " +
            "\"as is\", without warranties or conditions of any kind. The source code is at " +
            "https://github.com/awslabs/dynamodb-shell.",
        "Accept",
    ].join(" ");
    eq(rendered, expected, "the notice text is D-P14-4, word for word");
    check(!/&[a-z#0-9]+;/i.test(inner), "the notice text uses no character references");
    for (const lead of ["dynamodb-shell (ddbsh) is experimental software.", "Credentials.",
        "Command history.", "No warranty."]) {
        check(inner.includes(`>${lead}</strong>`), `"${lead}" is bold`);
    }
    check(/<a href="https:\/\/github\.com\/awslabs\/dynamodb-shell" target="_blank"\s+rel="noopener noreferrer">https:\/\/github\.com\/awslabs\/dynamodb-shell<\/a>/.test(inner),
        "the source-code link is a live link (new tab, noopener)");
    eq((inner.match(/<button\b/g) || []).length, 1, "the notice has one button");
    check(/<button id="notice-accept-btn" type="button" autofocus>Accept<\/button>/.test(inner),
        "the button is Accept, and focus moves to it");
    check(!/<form\b/.test(inner), "the notice has no form (no submission, CSP form-action 'none')");
    check(/aria-labelledby="notice-title"/.test(m ? m[0] : "") &&
        /aria-describedby="notice-body"/.test(m ? m[0] : ""), "the dialog is labelled and described for screen readers");

    // app.js: the checks run first, the notice holds the credentials, and
    // only Accept reaches the bridge.
    const app = readFileSync(new URL("./app.js", import.meta.url), "utf8");
    const use = app.slice(app.indexOf("function onUseCredentials()"), app.indexOf("function applyCredentials("));
    check(use.indexOf("credentialEnvironmentProblem") !== -1 &&
        use.indexOf("parseCredentials") < use.indexOf("notice.request(") &&
        use.indexOf("credentialEnvironmentProblem") < use.indexOf("notice.request("),
        "Use credentials runs the existing checks before the notice");
    check(!use.includes("setCredentials"), "Use credentials does not apply the credentials itself");
    eq((app.match(/bridge\.setCredentials\(/g) || []).length, 1, "the bridge gets credentials in one place");
    check(app.slice(app.indexOf("function applyCredentials(")).startsWith(
        "function applyCredentials(credentials) {\n    const set = bridge.setCredentials(credentials);"),
        "that place is applyCredentials, the notice's apply");
    check(/apply: \(credentials\) => applyCredentials\(credentials\)/.test(app), "the notice applies through applyCredentials");
    check(/els\.notice\.showModal\(\)/.test(app), "the notice is a modal dialog");
    check(/function onNoticeCancel\(ev\) \{\s*ev\.preventDefault\(\);/.test(app), "Escape does not dismiss the notice");
    check(/els\.notice\.addEventListener\("cancel", onNoticeCancel\)/.test(app), "the cancel handler is wired");
    check(/notice\.discard\(\)/.test(app.slice(app.indexOf("function onPageHide()"))), "pagehide drops held credentials");
    const wiring = app.slice(app.indexOf("const notice = createCredentialNotice"), app.indexOf("function onForgetCredentials"));
    check(!/localStorage|sessionStorage|indexedDB|cookie/.test(wiring), "the notice wiring touches no storage and no cookie");
}

// ---- summary ---------------------------------------------------------------
console.log(failures === 0
    ? "\napp-core tests: PASS"
    : `\napp-core tests: FAIL (${failures} assertion failure(s))`);
process.exit(failures === 0 ? 0 : 1);
