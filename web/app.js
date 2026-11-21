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
// D-P9-1). Credentials (D-P9-3 / D-P9-4):
//   * short-term only (ASIA… + session token); pasted into the credentials
//     panel, parsed + validated by app-core, held only inside the bridge;
//   * the textarea is cleared after every "Use credentials", whatever the
//     outcome; nothing credential-related is stored, logged or put in the URL;
//   * refused outside a secure context (https:// or http://localhost,
//     D-P9-02-7).
// The only localStorage use is the command history (editor submissions only).

import {
    buildPrompt,
    buildTranscriptEntries,
    credentialStatus,
    fileToSubmission,
    createHistory,
    createHistoryRecall,
    createRunGuard,
    parseCredentials,
} from "./app-core.mjs";
import { createSdkBridge, installHooks } from "./sdk-bridge.mjs";
import * as sdk from "./vendor/aws-sdk/aws-sdk-dynamodb.mjs";
import { ALLOWED_HOSTS, REGIONS, SDK_VERSION } from "./vendor/aws-sdk/allowed-hosts.mjs";

// ---- DOM handles -----------------------------------------------------------
const els = {
    fileBanner: document.getElementById("file-protocol-banner"),
    regionLabel: document.getElementById("region-label"),
    backendLabel: document.getElementById("backend-label"),
    backendDot: document.getElementById("backend-dot"),
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
    if (!window.isSecureContext) {
        setCredMessage("credentials can only be used on https:// or http://localhost; " +
            "serve over HTTPS or use an SSH tunnel", true);
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
    // Capture the prompt BEFORE running: a CONNECT inside the submission may
    // change the region, but the echo reflects the region as typed.
    const prompt = buildPrompt(session.region(), session.endpointOverridden());
    try {
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
        refreshRegion(); // CONNECT may have changed the region live.
        refreshCredStatus();
        setBusy(false);
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
function setBusy(busy) {
    els.runBtn.disabled = busy;
    els.fileInput.disabled = busy;
    els.fileLabel.setAttribute("aria-disabled", String(busy));
}

function setReady() {
    els.editor.disabled = false;
    els.runBtn.disabled = false;
    els.fileInput.disabled = false;
    els.fileLabel.setAttribute("aria-disabled", "false");
    els.backendLabel.textContent = `Amazon DynamoDB (AWS SDK for JavaScript ${SDK_VERSION})`;
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
    els.editor.addEventListener("keydown", onEditorKeydown);
    els.editor.addEventListener("input", onEditorInput);
    els.fileInput.addEventListener("change", onFileChosen);
    els.credUseBtn.addEventListener("click", onUseCredentials);
    els.credForgetBtn.addEventListener("click", onForgetCredentials);
    els.regionSelect.addEventListener("change", refreshRegion);

    if (!window.isSecureContext) {
        setCredMessage("this page is not in a secure context; credentials can only be used on " +
            "https:// or http://localhost (serve over HTTPS or use an SSH tunnel)", true);
    }

    try {
        const createDdbsh = (await import("./ddbsh.js")).default;
        Module = await createDdbsh();
        installHooks(Module, bridge);
        setReady();
    } catch (e) {
        els.backendLabel.textContent = "failed to load";
        els.backendDot.className = "dot";
        appendEntries([{ text: `failed to initialize ddbsh: ${e && e.message ? e.message : e}\n`,
                         kind: "stderr" }]);
    }
}

start();
