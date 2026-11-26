// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// app.js — the thin DOM wiring for the ddbsh browser edition (P6e UI; P9).
//
// This file owns ONLY the browser side: instantiate the WASM engine, create the
// AWS SDK bridge (sdk-bridge.mjs) and wire its hooks onto the module, run the
// credentials panel, bind editor events, call the DOM-free app-core.mjs for all
// real logic, and paint results with Text nodes and textContent (never
// innerHTML — D-P6e-6). No lexing, parsing, planning, execution or rendering
// happens here: the faithful shell text comes out of the engine's Renderer,
// delivered piece by piece by DdbshSession.runStreaming (tenet 3).
//
// Timely output (P12, D-P12-17): a submission's prompt echo is painted first,
// then each piece of output as the engine emits it. The page repaints while
// the engine waits for its next DynamoDB response, so SHOW TABLES row k is on
// screen while row k+1 is being described. The event callback touches only
// the transcript, never the module or the session (D-P6e-2).
//
// Output formats (P12, D-P12-26 / D-P12-25): two dropdowns by the Run button
// pick the output (Default, JSON, CSV, Table (text), Table (HTML)) and the JSON
// flavor; app-core's outputOptions turns them into runStreaming's options at
// submit. Table (HTML) arrives as rows events, and the page builds a <table>
// from them with createElement and textContent (the model is app-core's
// createTableModel); every other format is engine text.
//
// Throttling (P12, D-P12-18 / D-P12-19): the SDK bridge reports each retry
// the SDK decides to make; the busy indicator shows "throttled, retrying" (or
// "retrying") in its own color until that request settles.
//
// Requests go from this tab straight to Amazon DynamoDB: the engine serializes
// each request, the vendored AWS SDK for JavaScript signs and sends it (P9,
// D-P9-1). Credentials (D-P9-3 / D-P9-4; P10):
//   * short-term only (ASIA… + session token); pasted into the credentials
//     panel, parsed + validated by app-core, held only inside the bridge;
//   * the textarea is cleared after every "Use credentials", whatever the
//     outcome; nothing credential-related is stored, logged or put in the URL;
//   * the textarea and "Use credentials" ship disabled in index.html, so a
//     page whose script never runs never invites a paste; start() enables
//     them only in a secure context (https:// or http://localhost,
//     D-P9-02-7), outside any frame, and when no other page's script opened
//     this window (P10, G1; app-core credentialEnvironmentProblem).
//     "Use credentials" checks again and fails closed;
//   * wiped on pagehide, so the back/forward cache never restores them (P10).
//   * held, after the checks pass, until the user accepts the "experimental
//     software" notice (P14, B9; D-P14-2 / D-P14-3): a modal <dialog> shown on
//     every "Use credentials", with one Accept button and no other way out.
//     Nothing records that it was seen. Credentials that fail a check are
//     refused as before, without the notice.
// The only localStorage use is the command history (editor submissions only),
// which the "Clear history" button removes, in this tab and, through the
// storage event, in the page's other open tabs (P10, D-P10-6).
//
// The header (P10): the title carries the engine's version (versionLabel,
// D-P10-14); the backend label reads "Amazon DynamoDB" (D-P10-15); a busy
// indicator counts the seconds while a submission runs and announces its start
// and finish to screen readers (createBusyClock, D-P10-16); the Help link is
// static markup in index.html (D-P10-17).

import {
    JSON_FLAVOR_CHOICES,
    OUTPUT_CHOICES,
    buildPrompt,
    createBusyClock,
    createCredentialNotice,
    credentialEnvironmentProblem,
    credentialStatus,
    fileToSubmission,
    createHistory,
    createHistoryRecall,
    createRunGuard,
    createTranscriptAppender,
    historyRemovedElsewhere,
    jsonFlavorApplies,
    outputOptions,
    parseCredentials,
    streamSubmission,
    versionLabel,
} from "./app-core.mjs";
import { createSdkBridge, installHooks } from "./sdk-bridge.mjs";
import * as sdk from "./vendor/aws-sdk/aws-sdk-dynamodb.mjs";
import { ALLOWED_HOSTS, REGIONS, SDK_VERSION } from "./vendor/aws-sdk/allowed-hosts.mjs";

// ---- DOM handles -----------------------------------------------------------
const els = {
    fileBanner: document.getElementById("file-protocol-banner"),
    versionLabel: document.getElementById("version-label"),
    regionLabel: document.getElementById("region-label"),
    backendLabel: document.getElementById("backend-label"),
    backendDot: document.getElementById("backend-dot"),
    busy: document.getElementById("busy"),
    busyText: document.getElementById("busy-text"),
    busyStatus: document.getElementById("busy-status"),
    credStatus: document.getElementById("cred-status"),
    credPanel: document.getElementById("cred-panel"),
    credInput: document.getElementById("cred-input"),
    credUseBtn: document.getElementById("cred-use-btn"),
    credForgetBtn: document.getElementById("cred-forget-btn"),
    credMessage: document.getElementById("cred-message"),
    regionSelect: document.getElementById("region-select"),
    resultPane: document.getElementById("result-pane"),
    editor: document.getElementById("editor"),
    runBtn: document.getElementById("run-btn"),
    fileLabel: document.getElementById("file-label"),
    fileInput: document.getElementById("file-input"),
    outputSelect: document.getElementById("output-select"),
    flavorSelect: document.getElementById("flavor-select"),
    clearBtn: document.getElementById("clear-btn"),
    clearHistoryBtn: document.getElementById("clear-history-btn"),
    toolbarStatus: document.getElementById("toolbar-status"),
    notice: document.getElementById("notice"),
    noticeAcceptBtn: document.getElementById("notice-accept-btn"),
};

const HISTORY_KEY = "ddbsh.history";
const HISTORY_LIMIT = 500;
const DEFAULT_REGION = "us-east-1";

const guard = createRunGuard();
const history = createHistory({ limit: HISTORY_LIMIT, entries: loadHistory() });
const recall = createHistoryRecall(history);
let session = null; // the embind DdbshSession, created on first use (ensureSession).
let Module = null;  // the instantiated WASM module.
const bridge = createSdkBridge({
    sdk,
    allowedHosts: ALLOWED_HOSTS,
    regions: REGIONS,
    sdkVersion: SDK_VERSION,
    region: DEFAULT_REGION,
});

// The header's busy indicator (D-P10-16; retry states P12, D-P12-19).
// textContent and classList only (Trusted Types, D-P6e-6). performance.now()
// is monotonic, so a change of the system clock does not skew the count;
// animation frames, not a timer, drive the re-render (see createBusyClock).
const busyClock = createBusyClock({
    now: () => performance.now(),
    requestFrame: (fn) => window.requestAnimationFrame(fn),
    cancelFrame: (handle) => window.cancelAnimationFrame(handle),
    render: (text, state) => {
        els.busyText.textContent = text === null ? "" : text;
        els.busy.classList.toggle("busy-running", text !== null);
        els.busy.classList.toggle("busy-throttled", state === "throttled");
        els.busy.classList.toggle("busy-retrying", state === "retrying");
    },
    announce: (text) => {
        els.busyStatus.textContent = text;
    },
});

// Retries the SDK has decided to make, and the settling of each request
// (P12, D-P12-18). They arrive from the SDK's retry loop while the engine is
// suspended, and touch only the busy indicator.
bridge.onRetryEvent((ev) => {
    if (ev.type === "retry") {
        busyClock.setRetrying(ev.throttled ? "throttled" : "retrying");
    } else if (ev.type === "settled") {
        busyClock.setRetrying(null);
    }
});

// ---- history persistence (localStorage; editor submissions only) -----------
function loadHistory() {
    try {
        const raw = window.localStorage.getItem(HISTORY_KEY);
        const parsed = raw ? JSON.parse(raw) : [];
        return Array.isArray(parsed) ? parsed : [];
    } catch {
        return []; // private mode / parse error: start empty, never throw.
    }
}

function persistHistory() {
    try {
        window.localStorage.setItem(HISTORY_KEY, JSON.stringify(history.list()));
    } catch {
        // storage unavailable (private mode / quota): history is in-memory only.
    }
}

// "Clear history" (D-P10-6): forget the stored copy and the in-memory ring.
// Saving stays on; the next command starts a new history.
function onClearHistory() {
    history.clear();
    recall.reset();
    try {
        window.localStorage.removeItem(HISTORY_KEY);
    } catch {
        // storage unavailable: there was no stored copy to remove.
    }
    els.toolbarStatus.textContent = "history cleared";
    els.editor.focus();
}

// Another open tab of this page pressed "Clear history" (or cleared the
// origin's storage). Forget this tab's copy too: persistHistory writes the
// whole ring back after every command, so this tab's next command would
// otherwise restore the deleted statements.
function onStorageChanged(ev) {
    if (historyRemovedElsewhere(ev, HISTORY_KEY)) {
        history.clear();
        recall.reset();
        els.toolbarStatus.textContent = "history cleared in another tab";
    }
}

// ---- transcript painting (Text nodes and textContent only — D-P6e-6) -------
// Run `change` (which adds to the transcript), then keep the transcript
// scrolled to its end if it was there before; if the user has scrolled up to
// read, leave their position alone.
function paintTranscript(change) {
    const pane = els.resultPane;
    const atBottom = pane.scrollHeight - pane.clientHeight - pane.scrollTop < 40;
    const result = change();
    if (atBottom) {
        pane.scrollTop = pane.scrollHeight;
    }
    return result;
}

// The streaming transcript's text blocks are <pre> elements in the result
// pane. Text goes in as Text nodes (pre.append(text) creates one): never
// innerHTML, because item data is untrusted, and never `textContent +=`, which
// copies the whole block on every event.
//
// The `html` output's result sets are <table> elements (P12, D-P12-25), built
// with createElement and textContent only: the page's CSP requires Trusted
// Types with no policy (so no innerHTML) and allows styles from styles.css
// only (so classes, never a style attribute). A table block is
// {root, headRow, body}; root is a scrolling wrapper <div>.
function createDomTranscriptAppender() {
    const nodeOf = (block) => (block instanceof HTMLElement ? block : block.root);
    return createTranscriptAppender({
        createBlock: (kind) => paintTranscript(() => {
            const pre = document.createElement("pre");
            pre.className = kind;
            els.resultPane.appendChild(pre);
            return pre;
        }),
        appendText: (pre, text) => paintTranscript(() => pre.append(text)),
        // Clear (replaceChildren) detaches the blocks of a running submission.
        isAttached: (block) => nodeOf(block).parentNode === els.resultPane,
        createTable: () => paintTranscript(() => {
            const root = document.createElement("div");
            root.className = "result-table-wrap";
            const table = document.createElement("table");
            table.className = "result-table";
            const head = document.createElement("thead");
            const headRow = document.createElement("tr");
            const body = document.createElement("tbody");
            head.appendChild(headRow);
            table.append(head, body);
            root.appendChild(table);
            els.resultPane.appendChild(root);
            return { root, headRow, body };
        }),
        applyTablePlan: (t, plan) => paintTranscript(() => applyTablePlan(t, plan)),
    });
}

// Apply one createTableModel plan (app-core) to a result table.
function applyTablePlan({ headRow, body }, plan) {
    for (const name of plan.addHeaders) {
        const th = document.createElement("th");
        th.scope = "col";
        th.textContent = name;
        headRow.appendChild(th);
    }
    for (let r = 0; r < plan.padRows && r < body.rows.length; r += 1) {
        for (let c = 0; c < plan.padCells; c += 1) {
            const td = document.createElement("td");
            td.className = "missing";
            body.rows[r].appendChild(td);
        }
    }
    for (const cells of plan.rows) {
        const tr = document.createElement("tr");
        for (const cell of cells) {
            const td = document.createElement("td");
            td.className = cell.className;
            td.textContent = cell.text; // never innerHTML: the value is untrusted data.
            tr.appendChild(td);
        }
        body.appendChild(tr);
    }
}

// ---- output format dropdowns (P12, D-P12-26) ---------------------------------
// Read at submit; not persisted (the page's only storage is the command
// history). The JSON flavor does not apply to Default, so it is disabled then.
function fillSelect(select, choices) {
    for (const [value, label] of choices) {
        const opt = document.createElement("option");
        opt.value = value;
        opt.textContent = label;
        select.appendChild(opt);
    }
}

function refreshFlavorSelect() {
    els.flavorSelect.disabled = !jsonFlavorApplies(els.outputSelect.value);
}

// Append whole blocks (messages outside a submission's stream).
function appendEntries(entries) {
    paintTranscript(() => {
        for (const entry of entries) {
            const pre = document.createElement("pre");
            pre.className = entry.kind;
            pre.textContent = entry.text; // never innerHTML: the text may hold untrusted data.
            els.resultPane.appendChild(pre);
        }
    });
}

// ---- credentials panel -----------------------------------------------------
// The three facts credentialEnvironmentProblem decides on. Comparing
// window.top with window.self and reading window.opener are allowed even when
// the other window is cross-origin.
function windowEnvironment() {
    let framed = true;
    let opened = true;
    try {
        framed = window.top !== window.self;
        opened = window.opener !== null && window.opener !== undefined;
    } catch {
        // cannot tell: treat as framed / opened (fail closed).
    }
    return { secureContext: window.isSecureContext === true, framed, opened };
}

function refreshCredStatus() {
    els.credStatus.textContent = credentialStatus(bridge.credentialSummary());
}

function setCredMessage(text, isError) {
    els.credMessage.textContent = text;
    els.credMessage.className = isError ? "cred-message error" : "cred-message";
}

// The notice (P14, B9). request() holds the checked credentials and opens
// the dialog; Accept applies them (applyCredentials). The dialog is modal, so
// the rest of the page is inert while it is open.
const notice = createCredentialNotice({
    show: () => {
        if (!els.notice.open) {
            els.notice.showModal(); // focus moves to Accept (autofocus).
        }
    },
    apply: (credentials) => applyCredentials(credentials),
});

function onNoticeAccept() {
    els.notice.close("accept");
    notice.accept();
}

// Escape asks the dialog to close (the "cancel" event). The notice has no
// way out but Accept, so refuse it.
function onNoticeCancel(ev) {
    ev.preventDefault();
}

// A browser may close a modal dialog anyway (some close a dialog on a second
// Escape even when "cancel" is refused). If credentials are still held, the
// user has not accepted: open the notice again.
function onNoticeClose() {
    if (notice.pending) {
        els.notice.showModal();
    }
}

function onUseCredentials() {
    const text = els.credInput.value;
    // Clear the secret from the DOM whatever happens next, so it is not kept in
    // the page or restored by back/forward navigation.
    els.credInput.value = "";
    const problem = credentialEnvironmentProblem(windowEnvironment());
    if (problem !== null) {
        setCredMessage(problem, true); // fail closed, even if startup's check was bypassed.
        return;
    }
    const parsed = parseCredentials(text);
    if (!parsed.ok) {
        setCredMessage(parsed.message, true);
        return;
    }
    // Checked: hold them and show the notice. They take effect on Accept.
    notice.request(parsed.credentials);
}

// The rest of the old "Use credentials" path, run on Accept.
function applyCredentials(credentials) {
    const set = bridge.setCredentials(credentials);
    if (!set.ok) {
        setCredMessage(set.message, true);
        return;
    }
    ensureSession();
    setCredMessage("credentials accepted (kept in this tab's memory only)", false);
    refreshCredStatus();
    els.credPanel.open = false;
    els.editor.focus();
}

function onForgetCredentials() {
    els.credInput.value = "";
    bridge.clearCredentials();
    refreshCredStatus();
    setCredMessage("credentials forgotten", false);
}

// pagehide fires when the page is put into the back/forward cache (and on
// unload). Forgetting the credentials then means a restored page never has
// them; the message tells the user why on return.
function onPageHide() {
    els.credInput.value = "";
    notice.discard(); // credentials held behind the notice go too.
    if (els.notice.open) {
        els.notice.close();
    }
    if (bridge.credentialSummary() !== null) {
        bridge.clearCredentials();
        refreshCredStatus();
        setCredMessage("credentials were forgotten when you left the page; paste them again", false);
        els.credPanel.open = true;
    }
}

bridge.onCredentialProblem(({ kind, detail }) => {
    refreshCredStatus();
    els.credPanel.open = true;
    if (kind === "expired") {
        setCredMessage(`these credentials expired at ${detail}; paste new short-term credentials`, true);
        els.credStatus.textContent = "expired";
    } else {
        setCredMessage(`rejected by DynamoDB: ${detail}; paste new short-term credentials`, true);
        els.credStatus.textContent = `rejected by DynamoDB: ${detail}`;
    }
});

function fillRegionSelect() {
    for (const r of REGIONS) {
        const opt = document.createElement("option");
        opt.value = r;
        opt.textContent = r;
        if (r === DEFAULT_REGION) {
            opt.selected = true;
        }
        els.regionSelect.appendChild(opt);
    }
}

// Create the session on first use (a submission or the first credentials).
// The initial-region selector applies until then; afterwards CONNECT changes the
// region, so the selector is disabled.
function ensureSession() {
    if (session !== null || Module === null) {
        return;
    }
    const region = els.regionSelect.value || DEFAULT_REGION;
    bridge.connect(region, "");
    session = new Module.DdbshSession(region);
    els.regionSelect.disabled = true;
}

// ---- submission (serialized; Asyncify re-entrancy safe — D-P6e-2) ----------
async function submit(text, { addToHistory }) {
    if (typeof text !== "string" || text.trim() === "") {
        return;
    }
    if (Module === null) {
        return; // the engine has not loaded.
    }
    ensureSession();
    if (bridge.credentialSummary() === null) {
        // HELP and parse errors need no backend; anything that calls DynamoDB
        // fails with "no credentials: …" from the bridge.
        els.credPanel.open = true;
    }
    if (!guard.begin()) {
        return; // a submission is already in flight; ignore this one.
    }
    setBusy(true);
    try {
        // Capture the prompt BEFORE running: a CONNECT inside the submission
        // may change the region, but the echo reflects the region as typed.
        // streamSubmission paints the echo first, then each piece of output
        // as it arrives, then any internal error (P12, D-P12-17).
        const prompt = buildPrompt(session.region(), session.endpointOverridden());
        const options = outputOptions(els.outputSelect.value, els.flavorSelect.value);
        await streamSubmission({ session, prompt, text, options, appender: createDomTranscriptAppender() });
        // History keeps the input even when runStreaming itself threw
        // (streamSubmission reported it and returned null): the user typed
        // it, and recalling it to retry is the useful behavior (P12 N9).
        if (addToHistory) {
            history.push(text);
            persistHistory();
        }
    } catch (e) {
        // An unexpected JS failure outside the run (engine errors arrive as
        // stderr events; streamSubmission reports a failure of the run
        // itself). Surface it rather than losing it.
        appendEntries([{ text: `internal error: ${e && e.message ? e.message : e}\n`,
                         kind: "stderr" }]);
    } finally {
        guard.end();
        setBusy(false); // first, so the busy indicator clears even if a refresh below throws.
        refreshRegion(); // CONNECT may have changed the region live.
        refreshCredStatus();
    }
}

function refreshRegion() {
    if (session) {
        els.regionLabel.textContent = session.region() +
            (session.endpointOverridden() ? " (*)" : "");
    } else {
        els.regionLabel.textContent = els.regionSelect.value || DEFAULT_REGION;
    }
}

// ---- UI enable/disable -----------------------------------------------------
// setBusy(true) runs right after the run guard is claimed and setBusy(false)
// in submit()'s finally, so the busy indicator covers Run, Ctrl/Cmd-Enter and
// file runs, and clears on success, an engine error or an internal error.
function setBusy(busy) {
    els.runBtn.disabled = busy;
    els.fileInput.disabled = busy;
    els.fileLabel.setAttribute("aria-disabled", String(busy));
    if (busy) {
        busyClock.start();
    } else {
        busyClock.stop();
    }
}

function setReady() {
    els.editor.disabled = false;
    els.runBtn.disabled = false;
    els.fileInput.disabled = false;
    els.fileLabel.setAttribute("aria-disabled", "false");
    els.versionLabel.textContent = versionLabel(Module);
    els.backendLabel.textContent = "Amazon DynamoDB";
    els.backendDot.className = "dot dot-ready";
    refreshRegion();
    refreshCredStatus();
    els.credInput.focus();
}

// ---- editor event wiring ---------------------------------------------------
function caretOnFirstLine(ta) {
    return ta.value.lastIndexOf("\n", ta.selectionStart - 1) === -1;
}
function caretOnLastLine(ta) {
    return ta.value.indexOf("\n", ta.selectionEnd) === -1;
}

function runFromEditor() {
    // Don't consume the buffer while a submission is in flight: run() is
    // Asyncify-suspending, so the event loop is live during a submission and a
    // second Ctrl/Cmd-Enter would otherwise clear the editor before submit()'s
    // guard refuses the re-entrant call (D-P6e-2).
    if (guard.busy) {
        return;
    }
    const text = els.editor.value;
    if (text.trim() === "") {
        return;
    }
    els.editor.value = "";
    els.toolbarStatus.textContent = "";
    recall.reset();
    submit(text, { addToHistory: true });
}

function onEditorKeydown(ev) {
    // Ctrl/Cmd-Enter runs the whole buffer; plain Enter inserts a newline.
    if (ev.key === "Enter" && (ev.ctrlKey || ev.metaKey)) {
        ev.preventDefault();
        runFromEditor();
        return;
    }
    // ArrowUp/ArrowDown traverse history only at the buffer edges, so arrows
    // still move within a multi-line statement (app-core decides).
    if (ev.key === "ArrowUp" && caretOnFirstLine(els.editor)) {
        const value = recall.up(els.editor.value);
        if (value !== null) {
            ev.preventDefault();
            setEditorValue(value);
        }
        return;
    }
    if (ev.key === "ArrowDown" && caretOnLastLine(els.editor)) {
        const value = recall.down();
        if (value !== null) {
            ev.preventDefault();
            setEditorValue(value);
        }
    }
}

function setEditorValue(value) {
    els.editor.value = value;
    const len = value.length;
    els.editor.selectionStart = els.editor.selectionEnd = len;
}

function onEditorInput() {
    recall.reset(); // any edit abandons an in-progress history recall.
}

async function onFileChosen(ev) {
    const file = ev.target.files && ev.target.files[0];
    ev.target.value = ""; // allow re-selecting the same file later.
    if (!file) {
        return;
    }
    const text = await file.text();
    // A file runs as one submission (the -f/source equivalent) and is NOT added
    // to the command-history ring (it is a batch, not a typed command).
    submit(fileToSubmission(text), { addToHistory: false });
}

function onClear() {
    els.resultPane.replaceChildren(); // empties the transcript, not history.
    els.editor.focus();
}

// ---- startup ---------------------------------------------------------------
async function start() {
    // WASM + ES modules cannot load from file:// — show the guard banner and
    // stop before attempting the module fetch (D-P6e-4).
    if (window.location.protocol === "file:") {
        els.fileBanner.hidden = false;
        els.backendLabel.textContent = "unavailable";
        return;
    }

    fillRegionSelect();
    fillSelect(els.outputSelect, OUTPUT_CHOICES);
    fillSelect(els.flavorSelect, JSON_FLAVOR_CHOICES);
    refreshFlavorSelect();
    els.outputSelect.addEventListener("change", refreshFlavorSelect);
    els.runBtn.addEventListener("click", runFromEditor);
    els.clearBtn.addEventListener("click", onClear);
    els.clearHistoryBtn.addEventListener("click", onClearHistory);
    els.editor.addEventListener("keydown", onEditorKeydown);
    els.editor.addEventListener("input", onEditorInput);
    els.fileInput.addEventListener("change", onFileChosen);
    els.credUseBtn.addEventListener("click", onUseCredentials);
    els.credForgetBtn.addEventListener("click", onForgetCredentials);
    els.noticeAcceptBtn.addEventListener("click", onNoticeAccept);
    els.notice.addEventListener("cancel", onNoticeCancel);
    els.notice.addEventListener("close", onNoticeClose);
    els.regionSelect.addEventListener("change", refreshRegion);
    window.addEventListener("pagehide", onPageHide);
    window.addEventListener("storage", onStorageChanged);

    // The credentials controls ship disabled (index.html). Enable them only
    // where credentials are safe to use; where they would be at risk (framed,
    // opened by another page's script, or not a secure context) leave them
    // disabled and say why. onUseCredentials checks again.
    const problem = credentialEnvironmentProblem(windowEnvironment());
    if (problem === null) {
        els.credInput.disabled = false;
        els.credUseBtn.disabled = false;
    } else {
        setCredMessage(problem, true);
    }

    try {
        const createDdbsh = (await import("./ddbsh.js")).default;
        Module = await createDdbsh();
        installHooks(Module, bridge);
        setReady();
    } catch (e) {
        // Without the engine nothing can use credentials: disable the panel
        // again so it does not invite a paste.
        els.credInput.disabled = true;
        els.credUseBtn.disabled = true;
        els.backendLabel.textContent = "failed to load";
        els.backendDot.className = "dot";
        appendEntries([{ text: `failed to initialize ddbsh: ${e && e.message ? e.message : e}\n`,
                         kind: "stderr" }]);
    }
}

start();
