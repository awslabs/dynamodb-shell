// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// streaming.test.mjs — offline Node tests for the browser edition's streaming
// entry point, DdbshSession.runStreaming (P12, D-P12-17; design test 3).
//
// Runs the BUILT ddbsh.js/ddbsh.wasm with stub transport hooks on Module (no
// SDK, no network, no AWS account): Module.ddbshSend answers each request from
// a script and records how many runStreaming events had arrived when the
// engine sent it. That count is what proves timeliness: SHOW TABLES row k
// must reach the callback before the engine asks for row k+1's DescribeTable,
// so the page can paint it while that request is in flight. The tests also
// pin that runStreaming delivers exactly the text runScript and run return,
// in the same stdout/stderr order, and that app-core's guardedEventSink and
// streamSubmission keep a throwing callback from breaking the run.
//
// Zero dependencies: plain asserts + an exit code. Run from the staged build
// output (build-preset/wasm-release/web/), where ddbsh.js lives:
//   node streaming.test.mjs
// devtools/lib-web.sh web_offline_gates runs it.

import {
    buildTranscriptEntries,
    createTranscriptAppender,
    guardedEventSink,
    streamSubmission,
} from "./app-core.mjs";
import createDdbsh from "./ddbsh.js";

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

// ---- the stub transport ------------------------------------------------------
// One Module; each test installs its own responder and log. The hooks are the
// three web/js_sdk_transport.cpp calls (sdk-bridge.mjs installHooks).
const Module = await createDdbsh();
let responder = () => "{}";
let sends = [];
let events = []; // every event the current test's callback received, in order.
Module.ddbshSend = async (op, body) => {
    let name = "";
    try {
        name = JSON.parse(body).TableName || "";
    } catch {
        name = "";
    }
    sends.push({ op, name, eventsBefore: events.length });
    await new Promise((resolve) => setTimeout(resolve, 0)); // a real suspension.
    return responder(op, body, sends.length);
};
Module.ddbshConnect = (region) => (region === "eu-west-1" ? "eu-west-1 refused by the stub" : "");
Module.ddbshDescriptor = () => "wire/stub";

const RNF = (name) => JSON.stringify({
    __type: "com.amazonaws.dynamodb.v20120810#ResourceNotFoundException",
    message: `Requested resource not found: Table: ${name} not found`,
});

// Three tables over two ListTables pages, each described in full.
function showTablesResponder({ failPageTwo = false } = {}) {
    let listCalls = 0;
    return (op, body) => {
        const req = JSON.parse(body);
        if (op === "ListTables") {
            listCalls += 1;
            if (listCalls === 1) {
                return JSON.stringify({ TableNames: ["t1", "t2"], LastEvaluatedTableName: "t2" });
            }
            if (failPageTwo) {
                return JSON.stringify({
                    __type: "com.amazonaws.dynamodb.v20120810#InternalServerError",
                    message: "page two failed",
                });
            }
            return JSON.stringify({ TableNames: ["t3"] });
        }
        if (op === "DescribeTable") {
            if (req.TableName === "nosuch") {
                return RNF("nosuch");
            }
            return JSON.stringify({ Table: {
                TableName: req.TableName, TableStatus: "ACTIVE",
                TableId: `id-${req.TableName}`, TableArn: `arn:stub:${req.TableName}`,
            } });
        }
        if (op === "DescribeTimeToLive") {
            return JSON.stringify({ TimeToLiveDescription: { TimeToLiveStatus: "DISABLED" } });
        }
        return "{}";
    };
}

async function settle(r) {
    return r && typeof r.then === "function" ? await r : r;
}

// Run `text` through runStreaming on a fresh session; return the result, the
// events and the sends.
async function stream(text, responderFactory, onEvent = (ev) => events.push(ev)) {
    responder = responderFactory();
    sends = [];
    events = [];
    const session = new Module.DdbshSession("us-east-1");
    const result = await settle(session.runStreaming(text, {}, onEvent));
    const out = { result, events, sends };
    session.delete();
    return out;
}

// The buffered entry points on a fresh session, for comparison.
async function buffered(method, text, responderFactory) {
    responder = responderFactory();
    sends = [];
    events = [];
    const session = new Module.DdbshSession("us-east-1");
    const result = method === "runScript"
        ? await settle(session.runScript(text, false, {}))
        : await settle(session.run(text));
    const segments = [];
    for (const seg of result.segments) {
        segments.push({ stream: seg.stream, text: seg.text });
    }
    session.delete();
    return { segments, status: result.status, ok: result.ok };
}

// Adjacent same-stream text events merged, as RunResult.segments are.
function coalesce(evs) {
    const out = [];
    for (const ev of evs) {
        check(ev.type === "text", `event type is text (${ev.type})`);
        const last = out[out.length - 1];
        if (last && last.stream === ev.stream) {
            last.text += ev.text;
        } else {
            out.push({ stream: ev.stream, text: ev.text });
        }
    }
    return out;
}

// ---- SHOW TABLES: row k before row k+1's describes ------------------------------
console.log("runStreaming: SHOW TABLES timeliness");
{
    const { result, events: evs, sends: log } = await stream("show tables;", showTablesResponder);
    eq(log.map((s) => `${s.op}${s.name ? `:${s.name}` : ""}`), [
        "ListTables",
        "DescribeTable:t1", "DescribeTimeToLive:t1",
        "DescribeTable:t2", "DescribeTimeToLive:t2",
        "ListTables",
        "DescribeTable:t3", "DescribeTimeToLive:t3",
    ], "the engine's request order (ListTables read page by page)");
    const rowEvent = (name) => evs.findIndex((e) => e.stream === "stdout" && e.text.startsWith(`${name} | `));
    const sendOf = (op, name) => log.find((s) => s.op === op && s.name === name);
    check(rowEvent("t1") >= 0 && rowEvent("t2") >= 0 && rowEvent("t3") >= 0, "one stdout event per table row");
    eq(evs.length, 3, "exactly three events (one per row)");
    check(rowEvent("t1") < sendOf("DescribeTable", "t2").eventsBefore,
        "row t1 reached the callback before the DescribeTable of t2 was sent");
    check(rowEvent("t2") < log[5].eventsBefore,
        "row t2 reached the callback before ListTables page 2 was requested");
    check(rowEvent("t3") >= sendOf("DescribeTimeToLive", "t3").eventsBefore,
        "row t3 arrived after its own describes (it needs their responses)");
    eq(log[0].eventsBefore, 0, "nothing was emitted before the first request");
    check(evs.every((e) => e.text.endsWith("\n")), "each row event is a whole line");
    eq([result.ok, result.status, result.stdout, result.stderr, result.segments.length],
        [true, 0, "", "", 0], "the result is {ok, status} with empty text and no segments");
}

// ---- the same text, in the same order, as runScript and run ----------------------
console.log("runStreaming: same output as runScript and run");
{
    // Rows, a failing statement, a CONNECT the transport rejects (its notice
    // follows its CONNECT token), a CONNECT it accepts, SHOW CONNECTION, and a
    // trailing parse error.
    const text = "show tables; describe nosuch; connect eu-west-1; connect us-west-2; " +
        "show connection; select * from;";
    const streamed = await stream(text, showTablesResponder);
    const viaScript = await buffered("runScript", text, showTablesResponder);
    const viaRun = await buffered("run", text, showTablesResponder);
    const merged = coalesce(streamed.events);
    eq(merged, viaScript.segments, "coalesced events equal runScript's segments");
    eq(merged, viaRun.segments, "coalesced events equal run's segments");
    eq(merged.map((s) => s.stream), ["stdout", "stderr", "stdout", "stderr", "stdout", "stderr"],
        "stdout / stderr interleaving: rows, the error, CONNECT, its notice, CONNECT + SHOW CONNECTION, the parse error");
    check(merged[1].text.includes("nosuch"), "the failing statement's error follows the rows");
    eq(merged[3].text, "browser: CONNECT rejected: eu-west-1 refused by the stub\n",
        "the rejected CONNECT's notice follows its CONNECT token");
    check(/syntax error/.test(merged[5].text), "the trailing parse error comes last");
    eq([streamed.result.ok, streamed.result.status], [viaRun.ok, viaRun.status],
        "status matches run() (last executed statement)");
}

// ---- a failure on a later page: earlier rows stay, then the error ----------------
console.log("runStreaming: partial output, then the error");
{
    const { result, events: evs, sends: log } = await stream("show tables;",
        () => showTablesResponder({ failPageTwo: true }));
    eq(evs.map((e) => e.stream), ["stdout", "stdout", "stderr"], "rows t1 and t2, then one stderr event");
    check(evs[2].text.includes("page two failed"), `the error names the failure (${JSON.stringify(evs[2].text)})`);
    check(evs[1].text.startsWith("t2 | ") && log[log.length - 1].eventsBefore === 2,
        "both rows were delivered before page 2 was requested");
    eq([result.ok, result.status], [false, 1], "the failed statement sets status 1");
}

// ---- the callback contract ---------------------------------------------------------
console.log("runStreaming: callback contract");
{
    responder = showTablesResponder();
    sends = [];
    const session = new Module.DdbshSession("us-east-1");
    const r = await settle(session.runStreaming("show tables;", {}, "not a function"));
    eq([r.ok, r.status, r.stderr], [false, 1, "runStreaming: onEvent is not a function\n"],
        "a non-function callback runs nothing and says why");
    eq(sends.length, 0, "  … and sends no request");
    session.delete();
}
{
    // guardedEventSink: the first event's callback throws; the run completes,
    // later events still arrive, and the first exception is kept.
    let calls = 0;
    const delivered = [];
    const guarded = guardedEventSink((ev) => {
        calls += 1;
        if (calls === 1) {
            throw new Error("paint failed");
        }
        if (calls === 2) {
            throw new Error("second failure");
        }
        delivered.push(ev.text);
    });
    const { result } = await stream("show tables;", showTablesResponder, guarded.emit);
    eq([result.ok, result.status], [true, 0], "the run completes despite a throwing callback");
    eq(calls, 3, "every event was offered to the callback");
    eq(delivered.length, 1, "the event after the failures was delivered");
    eq(guarded.error && guarded.error.message, "paint failed", "the first exception is kept");
}

// ---- streamSubmission: prompt first, events in order, internal error last ---------
console.log("streamSubmission");
{
    // A DOM stand-in: blocks are {kind, parts, attached}.
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
    responder = showTablesResponder();
    sends = [];
    const firstSendBlocks = [];
    const realSend = Module.ddbshSend;
    Module.ddbshSend = async (op, body) => {
        firstSendBlocks.push(blocks.map((b) => b.kind).join(","));
        return realSend(op, body);
    };
    const session = new Module.DdbshSession("us-east-1");
    const text = "show tables;\ndescribe nosuch;";
    const status = await streamSubmission({ session, prompt: "us-east-1> ", text, appender });
    Module.ddbshSend = realSend;
    eq(firstSendBlocks[0], "prompt", "the prompt echo is in the transcript before the first request");
    eq(blocks.map((b) => b.kind), ["prompt", "stdout", "stderr"], "blocks: prompt, rows, error");
    eq(blocks[0].parts.join(""), "us-east-1> show tables;\nus-east-1> describe nosuch;",
        "the prompt block echoes each input line");
    eq(blocks[1].parts.length, 3, "the three rows were appended one at a time to one block");
    eq(status, { ok: false, status: 1 }, "the run's status is returned");

    // The same transcript as the buffered path builds.
    responder = showTablesResponder();
    const other = new Module.DdbshSession("us-east-1");
    const ref = await settle(other.run(text));
    const entries = buildTranscriptEntries("us-east-1> ", text, ref);
    eq(blocks.map((b) => ({ text: b.parts.join(""), kind: b.kind })), entries,
        "the streamed transcript equals buildTranscriptEntries for run()'s result");

    // A paint failure mid-run: reported after the run as an internal error.
    blocks.length = 0;
    responder = showTablesResponder();
    let n = 0;
    const flaky = createTranscriptAppender({
        createBlock: (kind) => {
            const b = { kind, parts: [], attached: true };
            blocks.push(b);
            return b;
        },
        appendText: (b, t) => {
            n += 1;
            if (n === 3) {
                throw new Error("DOM refused the text");
            }
            b.parts.push(t);
        },
        isAttached: (b) => b.attached,
    });
    const s2 = await streamSubmission({ session, prompt: "p> ", text: "show tables;", appender: flaky });
    eq(s2, { ok: true, status: 0 }, "the run completes after a paint failure");
    eq(blocks.map((b) => b.kind), ["prompt", "stdout", "stderr"], "prompt, rows, then the internal error");
    eq(blocks[2].parts.join(""), "internal error: DOM refused the text\n",
        "the first callback exception is reported after the run");
    eq(blocks[1].parts.length, 2, "the other rows were still painted");

    // runStreaming itself throwing: reported, null returned.
    blocks.length = 0;
    const broken = { runStreaming() { throw new Error("module gone"); } };
    eq(await streamSubmission({ session: broken, prompt: "p> ", text: "x;", appender: flaky }), null,
        "a throwing runStreaming returns null");
    eq(blocks.map((b) => [b.kind, b.parts.join("")]), [["prompt", "p> x;"], ["stderr", "internal error: module gone\n"]],
        "  … after the prompt echo, with the reason");
    session.delete();
    other.delete();
}

// ---- output formats (P12 S5, D-P12-17 / D-P12-25) ---------------------------------

// A table `t` (pk number HASH) whose Scan returns two pages; page 2 adds the
// attribute `b`, and its second item has only the key. With `empty`, one empty page; with `failPageTwo`, page 2 is
// an InternalServerError.
function selectResponder({ empty = false, failPageTwo = false } = {}) {
    let scans = 0;
    return (op) => {
        if (op === "DescribeTable") {
            return JSON.stringify({ Table: {
                TableName: "t", TableStatus: "ACTIVE",
                KeySchema: [{ AttributeName: "pk", KeyType: "HASH" }],
                AttributeDefinitions: [{ AttributeName: "pk", AttributeType: "N" }],
            } });
        }
        if (op === "Scan") {
            scans += 1;
            const cc = { TableName: "t", CapacityUnits: 0.5 };
            if (empty) {
                return JSON.stringify({ Items: [], Count: 0, ScannedCount: 0, ConsumedCapacity: cc });
            }
            if (scans % 2 === 1) {
                return JSON.stringify({ Items: [{ pk: { N: "1" }, a: { S: "x\ny" } }],
                    LastEvaluatedKey: { pk: { N: "1" } }, ConsumedCapacity: cc });
            }
            if (failPageTwo) {
                return JSON.stringify({
                    __type: "com.amazonaws.dynamodb.v20120810#InternalServerError",
                    message: "page two failed",
                });
            }
            return JSON.stringify({ Items: [{ pk: { N: "2" }, a: { S: "z" }, b: { M: { k: { N: "3" } } } },
                { pk: { N: "3" } }], ConsumedCapacity: cc });
        }
        return "{}"; // PutItem
    };
}

async function streamWith(text, options, responderFactory) {
    responder = responderFactory();
    sends = [];
    events = [];
    const session = new Module.DdbshSession("us-east-1");
    const result = await settle(session.runStreaming(text, options, (ev) => events.push(ev)));
    const out = { result, events, sends };
    session.delete();
    return out;
}

async function scriptWith(text, options, responderFactory) {
    responder = responderFactory();
    sends = [];
    events = [];
    const session = new Module.DdbshSession("us-east-1");
    const result = await settle(session.runScript(text, false, options));
    const segments = [];
    for (const seg of result.segments) {
        segments.push({ stream: seg.stream, text: seg.text });
    }
    session.delete();
    return { segments, status: result.status };
}

console.log("runStreaming: every format's events equal runScript's bytes");
{
    const inputs = [
        ["show tables; describe nosuch; connect eu-west-1; show tables like \"t[13]\"; select * from;",
            showTablesResponder],
        ["select * from t return consumed capacity total; insert into t (pk) values (5); select * from t;",
            selectResponder],
        ["select * from t;", () => selectResponder({ empty: true })],
        ["select * from t; select * from t;", () => selectResponder({ failPageTwo: true })],
    ];
    for (const output of ["default", "json", "csv", "table"]) {
        for (const jsonFlavor of ["plain", "dynamodb"]) {
            for (const [text, factory] of inputs) {
                const options = { output, jsonFlavor };
                const streamed = await streamWith(text, options, factory);
                const script = await scriptWith(text, options, factory);
                const label = `${output}/${jsonFlavor}: ${text.slice(0, 32)}…`;
                eq(coalesce(streamed.events), script.segments, `${label} events equal runScript's segments`);
                eq(streamed.result.status, script.status, `${label} same status`);
            }
        }
    }
    // A sanity check that the comparison saw formatted bytes, not default ones.
    const json = await streamWith("select * from t;", { output: "json" }, selectResponder);
    eq(json.events.map((e) => e.text).join(""),
        '[\n  {"a": "x\\ny", "pk": 1},\n  {"a": "z", "b": {"k": 3}, "pk": 2},\n  {"pk": 3}\n]\n',
        "json: one array over both pages, one element per row");
}

console.log("runStreaming: html rows events");
{
    const { result, events: evs, sends: log } = await streamWith(
        "select * from t return consumed capacity total; insert into t (pk) values (5);",
        { output: "html" }, selectResponder);
    eq(evs.map((e) => e.type), ["rows", "rows", "end", "text", "text"],
        "two rows events (one per page; the empty capacity-only fragment sends none), end, then the capacity line and the INSERT token");
    eq([evs[0].columns, evs[0].newColumnsFrom, evs[0].groupEnd], [["pk", "a"], 0, true],
        "page 1: the key column first (the hint), then the page's other attributes");
    eq(evs[0].rows, [[{ t: "1", n: true, m: false }, { t: "x\ny", n: false, m: false }]],
        "page 1's cells: number flagged, text not escaped");
    eq([evs[1].columns, evs[1].newColumnsFrom], [["pk", "a", "b"], 2],
        "page 2 adds `b`: the full column list, newColumnsFrom = the old column count");
    eq(evs[1].rows, [
        [{ t: "2", n: true, m: false }, { t: "z", n: false, m: false }, { t: '{"k": 3}', n: false, m: false }],
        [{ t: "3", n: true, m: false }, { t: "", n: false, m: true }, { t: "", n: false, m: true }],
    ], "page 2's cells: a map as plain JSON; a missing attribute is an empty m cell");
    check(/^SELECT \(t, /.test(evs[3].text) && evs[3].stream === "stdout", "the capacity line follows the end");
    eq([evs[4].stream, evs[4].text], ["stdout", "INSERT\n"], "then the INSERT token");
    const scans = log.filter((s) => s.op === "Scan");
    eq(scans[1].eventsBefore, 1, "page 1's rows event arrived before page 2 was requested");
    eq([result.ok, result.status, result.stderr, result.segments.length], [true, 0, "", 0],
        "the result is {ok, status} only");
}
{
    const typed = await streamWith("select * from t;", { output: "html", jsonFlavor: "dynamodb" }, selectResponder);
    eq(typed.events[1].rows[0][2], { t: '{"M": {"k": {"N": "3"}}}', n: false, m: false },
        "html + dynamodb flavor: a map cell in the typed encoding (format_cell)");
}
{
    // A mid-stream error closes the set before the error.
    const { result, events: evs } = await streamWith("select * from t;", { output: "html" },
        () => selectResponder({ failPageTwo: true }));
    eq(evs.map((e) => e.type), ["rows", "end", "text"], "a failure on page 2: rows, end, then the error");
    eq(evs[2].stream, "stderr", "  … the error is a stderr text event");
    check(evs[2].text.includes("page two failed"), "  … naming the failure");
    eq([result.ok, result.status], [false, 1], "  … and status 1");
}
{
    const { events: evs } = await streamWith("select * from t;", { output: "html" },
        () => selectResponder({ empty: true }));
    eq(evs, [{ type: "rows", columns: ["pk"], newColumnsFrom: 0, rows: [], groupEnd: true }, { type: "end" }],
        "an empty SELECT: a rows event with the hint column and no rows, then end");
}
{
    // SHOW TABLES: report columns; a row whose describe failed has missing cells.
    const { events: evs } = await streamWith("show tables;", { output: "html" }, showTablesResponder);
    eq(evs.map((e) => e.type), ["rows", "rows", "rows", "end"], "SHOW TABLES: one rows event per table, then end");
    eq(evs[0].columns.slice(0, 3), ["TableName", "TableStatus", "BillingMode"], "the report's fixed columns");
    eq(evs[0].rows[0][0], { t: "t1", n: false, m: false }, "the first cell is the table name");
    eq(evs[2].newColumnsFrom, evs[2].columns.length, "a later row adds no column");
}
{
    // The open-set boundaries: each JSON array of a run is one html rows…end run.
    const text = "select * from t return consumed capacity total; insert into t (pk) values (5); select * from t;";
    const json = await streamWith(text, { output: "json" }, selectResponder);
    const html = await streamWith(text, { output: "html" }, selectResponder);
    // A JSON writer's pieces: "[\n  {…}" opens, ",\n  {…}" continues, "\n]\n"
    // closes, "[]\n" opens and closes; anything else is a text part.
    const jsonShape = [];
    for (const e of json.events) {
        if (e.text.startsWith("[\n")) {
            jsonShape.push("open");
        } else if (e.text === "[]\n") {
            jsonShape.push("open", "close");
        } else if (e.text === "\n]\n") {
            jsonShape.push("close");
        } else if (!e.text.startsWith(",\n")) {
            jsonShape.push("text");
        }
    }
    let open = false;
    const htmlShape = [];
    for (const e of html.events) {
        if (e.type === "rows" && !open) {
            open = true;
            htmlShape.push("open");
        } else if (e.type === "end") {
            open = false;
            htmlShape.push("close");
        } else if (e.type === "text") {
            htmlShape.push("text");
        }
    }
    eq(htmlShape, jsonShape, "html rows/end runs open and close where the JSON arrays do");
    eq(htmlShape, ["open", "close", "text", "text", "open", "close"], "  … rows, capacity, INSERT, rows");
}

console.log("runStreaming / runScript: option errors");
{
    const { result, events: evs, sends: log } = await streamWith("show tables;", { output: "xml" }, showTablesResponder);
    eq([result.ok, result.status], [false, 1], "an unknown output: status 1");
    eq(evs.map((e) => [e.type, e.stream]), [["text", "stderr"]], "  … one stderr event");
    check(evs[0].text.startsWith("unknown output format: xml"), `  … with the CLI's message (${JSON.stringify(evs[0].text)})`);
    eq(log.length, 0, "  … and no request was sent");
}
{
    const { result, events: evs, sends: log } = await streamWith("show tables;", { output: "json", jsonFlavor: "typed" },
        showTablesResponder);
    eq([result.status, log.length], [1, 0], "an unknown jsonFlavor: status 1, nothing sent");
    check(evs.length === 1 && /typed/.test(evs[0].text), "  … the message names the value");
}
{
    const script = await scriptWith("show tables;", { output: "html" }, showTablesResponder);
    eq(script.status, 1, "runScript rejects html (web-only, runStreaming's)");
    check(script.segments.length === 1 && script.segments[0].text.startsWith("unknown output format: html"),
        "  … with the CLI's message");
    const upper = await streamWith("select * from t;", { output: "HTML" }, selectResponder);
    eq(upper.events.map((e) => e.type), ["rows", "rows", "end"], "runStreaming accepts HTML in any case, as the CLI ids");
}

if (failures > 0) {
    console.error(`\nstreaming tests: ${failures} FAILED`);
    process.exit(1);
}
console.log("\nstreaming tests: all passed");
process.exit(0);
