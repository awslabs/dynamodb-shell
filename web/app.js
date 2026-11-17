// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// app.js — the thin DOM wiring for the P6e ddbsh web UI.
//
// This file owns ONLY the browser side: instantiate the WASM modules, wire the
// Module.submit bridge to the vendored in-browser ExtendDB adapter, bind DOM
// events, call into the DOM-free app-core.mjs for all real logic, and paint the
// result with textContent (never innerHTML — D-P6e-6). No lexing, parsing,
// planning, execution, or rendering happens here: the faithful shell text comes
// out of the engine's Renderer in RunResult.stdout/stderr (tenet 3, O-2).
//
// Startup, the Asyncify await pattern, the serve-over-HTTP requirement, and the
// supported/unsupported op map are described in web/README.md.

import createDdbsh from "./ddbsh.js";
import initExtendDb, { ExtendDb } from "./vendor/extenddb/extenddb_wasm_adapter.js";

import {
    buildPrompt,
    buildTranscriptEntries,
    fileToSubmission,
    createHistory,
    createHistoryRecall,
    createRunGuard,
} from "./app-core.mjs";

// ---- DOM handles -----------------------------------------------------------
const els = {
    fileBanner: document.getElementById("file-protocol-banner"),
    regionLabel: document.getElementById("region-label"),
    backendLabel: document.getElementById("backend-label"),
    backendDot: document.getElementById("backend-dot"),
    resultPane: document.getElementById("result-pane"),
    editor: document.getElementById("editor"),
    runBtn: document.getElementById("run-btn"),
    fileLabel: document.getElementById("file-label"),
    fileInput: document.getElementById("file-input"),
    clearBtn: document.getElementById("clear-btn"),
};

const HISTORY_KEY = "ddbsh.history";
const HISTORY_LIMIT = 500;
const REGION = "us-east-1";

const guard = createRunGuard();
const history = createHistory({ limit: HISTORY_LIMIT, entries: loadHistory() });
const recall = createHistoryRecall(history);
let session = null;       // the embind DdbshSession, once the modules are up.

// ---- history persistence (localStorage; app-core stays storage-free) -------
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

// ---- submission (serialized; Asyncify re-entrancy safe — D-P6e-2) ----------
async function submit(text, { addToHistory }) {
    if (!session || typeof text !== "string" || text.trim() === "") {
        return;
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
        // An unexpected JS/bridge failure (not an engine error — those come back
        // in RunResult.stderr). Surface it in the error style rather than losing it.
        appendEntries([{ text: `internal error: ${e && e.message ? e.message : e}\n`,
                         kind: "stderr" }]);
    } finally {
        guard.end();
        refreshRegion(); // CONNECT may have restitched the region live.
        setBusy(false);
    }
}

function refreshRegion() {
    if (session) {
        els.regionLabel.textContent = session.region() +
            (session.endpointOverridden() ? " (*)" : "");
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
    els.backendLabel.textContent = "ExtendDB";
    els.backendDot.className = "dot dot-ready";
    refreshRegion();
    els.editor.focus();
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
    // guard refuses the re-entrant call, silently losing the just-typed text
    // (D-P6e-2).
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
    // still move within a multi-line statement. The recall state machine
    // (app-core) decides whether to rewrite the buffer or let the key pass
    // through; a null result means "leave the buffer alone".
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
        return;
    }
}

function setEditorValue(value) {
    els.editor.value = value;
    const len = value.length;
    els.editor.selectionStart = els.editor.selectionEnd = len;
}

function onEditorInput() {
    // Any edit abandons an in-progress history recall.
    recall.reset();
}

async function onFileChosen(ev) {
    const file = ev.target.files && ev.target.files[0];
    ev.target.value = ""; // allow re-selecting the same file later.
    if (!file) {
        return;
    }
    const text = await file.text();
    // A file runs as one submission (the -f/source equivalent) and is NOT
    // added to the command-history ring (it is a batch, not a typed command).
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

    // Bind events now; controls stay disabled until the modules are ready.
    els.runBtn.addEventListener("click", runFromEditor);
    els.clearBtn.addEventListener("click", onClear);
    els.editor.addEventListener("keydown", onEditorKeydown);
    els.editor.addEventListener("input", onEditorInput);
    els.fileInput.addEventListener("change", onFileChosen);

    try {
        await initExtendDb();               // browser: fetch sibling _bg.wasm over HTTP.
        const db = await ExtendDb.create();  // async migrations (in-memory SQLite).
        const Module = await createDdbsh();  // ddbsh.wasm up; embind registered.
        Module.submit = (op, body) => db.submit(op, body); // wire the Asyncify bridge.
        session = new Module.DdbshSession(REGION);
        setReady();
    } catch (e) {
        els.backendLabel.textContent = "failed to load";
        els.backendDot.className = "dot";
        appendEntries([{ text: `failed to initialize ddbsh: ${e && e.message ? e.message : e}\n`,
                         kind: "stderr" }]);
    }
}

start();
