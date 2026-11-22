// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// app.js — the thin DOM wiring for the ddbsh browser edition (P6e UI; P9).
//
// This file owns ONLY the browser side: instantiate the WASM engine, create the
// AWS SDK bridge (sdk-bridge.mjs) and wire its hooks onto the module, run the
// credentials panel, bind editor events, call the DOM-free app-core.mjs for all
// real logic, and paint results with textContent (never innerHTML — D-P6e-6).
// No lexing, parsing, planning, execution or rendering happens here: the
// faithful shell text comes out of the engine's Renderer in RunResult.segments
// (tenet 3).
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
    buildPrompt,
    buildTranscriptEntries,
    createBusyClock,
    credentialEnvironmentProblem,
    credentialStatus,
    fileToSubmission,
    createHistory,
    createHistoryRecall,
    createRunGuard,
    historyRemovedElsewhere,
    parseCredentials,
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
    clearBtn: document.getElementById("clear-btn"),
    clearHistoryBtn: document.getElementById("clear-history-btn"),
    toolbarStatus: document.getElementById("toolbar-status"),
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

// The header's busy indicator (D-P10-16). textContent and classList only
// (Trusted Types, D-P6e-6). performance.now() is monotonic, so a change of the
// system clock does not skew the count; animation frames, not a timer, drive
// the re-render (see createBusyClock).
const busyClock = createBusyClock({
    now: () => performance.now(),
    requestFrame: (fn) => window.requestAnimationFrame(fn),
    cancelFrame: (handle) => window.cancelAnimationFrame(handle),
    render: (text) => {
        els.busyText.textContent = text === null ? "" : text;
        els.busy.classList.toggle("busy-running", text !== null);
    },
    announce: (text) => {
        els.busyStatus.textContent = text;
    },
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

// ---- transcript painting (textContent only — D-P6e-6) ----------------------
function appendEntries(entries) {
    if (entries.length === 0) {
        return;
    }
    const pane = els.resultPane;
    // Preserve the user's scroll position if they have scrolled up to read.
    const atBottom = pane.scrollHeight - pane.clientHeight - pane.scrollTop < 40;
    for (const entry of entries) {
        const pre = document.createElement("pre");
        pre.className = entry.kind;
        pre.textContent = entry.text; // never innerHTML: item data is untrusted.
        pane.appendChild(pre);
    }
    if (atBottom) {
        pane.scrollTop = pane.scrollHeight;
    }
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
    const set = bridge.setCredentials(parsed.credentials);
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
        const prompt = buildPrompt(session.region(), session.endpointOverridden());
        let result = session.run(text);
        if (result && typeof result.then === "function") {
            result = await result; // Asyncify may surface run() as a Promise.
        }
        appendEntries(buildTranscriptEntries(prompt, text, result));
        if (addToHistory) {
            history.push(text);
            persistHistory();
        }
    } catch (e) {
        // An unexpected JS/bridge failure (engine errors come back in the
        // result segments). Surface it rather than losing it.
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
    els.runBtn.addEventListener("click", runFromEditor);
    els.clearBtn.addEventListener("click", onClear);
    els.clearHistoryBtn.addEventListener("click", onClearHistory);
    els.editor.addEventListener("keydown", onEditorKeydown);
    els.editor.addEventListener("input", onEditorInput);
    els.fileInput.addEventListener("change", onFileChosen);
    els.credUseBtn.addEventListener("click", onUseCredentials);
    els.credForgetBtn.addEventListener("click", onForgetCredentials);
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
