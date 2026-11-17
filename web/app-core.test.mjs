// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// app-core.test.mjs — Node unit tests for the DOM-free P6e UI logic (D-P6e-3).
//
// Zero dependencies (plain asserts + an exit code, like web/harness.mjs), no
// DOM, no WASM: run directly with `node web/app-core.test.mjs`, or via
// devtools/run-web-smoke alongside the engine smoke. Covers the genuinely new
// P6e logic — the history ring, the prompt builder, the transcript-entry model,
// the file->submission handoff, and the single-flight run guard.

import {
    buildPrompt,
    buildTranscriptEntries,
    fileToSubmission,
    createHistory,
    createHistoryRecall,
    createRunGuard,
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

// ---- summary ---------------------------------------------------------------
console.log(failures === 0
    ? "\napp-core tests: PASS"
    : `\napp-core tests: FAIL (${failures} assertion failure(s))`);
process.exit(failures === 0 ? 0 : 1);
