// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// app.js — the thin DOM wiring for the ddbsh web UI (P6e UI; P6f-e two-edition
// wiring).
//
// This file owns ONLY the browser side: pick the edition, instantiate the WASM
// module, wire the transport bridge to the backend, bind DOM events, call into
// the DOM-free app-core.mjs for all real logic, and paint the result with
// textContent (never innerHTML — D-P6e-6). No lexing, parsing, planning,
// execution, or rendering happens here: the faithful shell text comes out of the
// engine's Renderer in RunResult.stdout/stderr (tenet 3, O-2).
//
// Two editions, one UI (D-P6f-2), auto-selected at startup by probing the
// companion proxy's /healthz:
//
//   * PROXY edition (served by `ddbsh-web-proxy`): loads ddbsh.js
//     (FetchTransport) and wires `Module.fetchDdb` / `Module.connectDdb` to POST
//     to the proxy, which SigV4-signs and forwards to REAL DynamoDB / ExtendDB.
//     Credentials never reach the browser — the proxy holds them. Per-endpoint
//     credential wiring (a named AWS profile + a CA bundle for a non-AWS backend
//     such as a local ExtendDB) is read from the page's `?profile=` / `?ca_bundle=`
//     query parameters and attached to endpoint-override CONNECTs.
//
//   * IN-TAB edition (served statically, no proxy): loads ddbsh-extenddb.js
//     (SubmitTransport) and wires `Module.submit` to the dropped-in in-browser
//     ExtendDB adapter. No server, no network, no credentials (pre-release).
//
// Startup, the Asyncify await pattern, the serve-over-HTTP requirement, and the
// supported/unsupported op map are described in web/README.md.

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

// Per-endpoint credential wiring for the proxy edition (read from the page URL).
// Neither is a secret: a profile NAME and a CA file PATH. The proxy resolves the
// actual key from that named ~/.aws profile; it never reaches the browser.
const params = new URLSearchParams(window.location.search);
const endpointConfig = {
    profile: params.get("profile") || "",
    caBundle: params.get("ca_bundle") || "",
};

const guard = createRunGuard();
const history = createHistory({ limit: HISTORY_LIMIT, entries: loadHistory() });
const recall = createHistoryRecall(history);
let session = null;         // the embind DdbshSession, once the module is up.
let backendName = "loading…"; // the connection-panel backend label for this edition.

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
    els.backendLabel.textContent = backendName;
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

// ---- proxy edition wiring (fetch transport -> ddbsh-web-proxy) -------------
// A same-origin POST to the proxy. The browser sets Origin and Host, which the
// proxy's driver guard requires; the session headers authenticate the request.
async function proxyPost(path, { headers = {}, body = null } = {}) {
    const res = await fetch(path, { method: "POST", headers, body, cache: "no-store" });
    const text = await res.text();
    return { status: res.status, text };
}

// Probe the companion proxy. Served by ddbsh-web-proxy -> /healthz returns
// {ok:true}; served statically -> 404 or a network error. Either non-ok result
// selects the in-tab edition.
async function proxyPresent() {
    try {
        const res = await fetch("healthz", { method: "GET", cache: "no-store" });
        if (!res.ok) {
            return false;
        }
        const j = await res.json().catch(() => null);
        return !!(j && j.ok === true);
    } catch {
        return false;
    }
}

// Bring up the proxy (real-backend) edition. Mirrors harness-proxy-lib.mjs's
// browser handshake + bridge wiring, using the browser `fetch` (which sets the
// Origin/Host the proxy guard needs) instead of node:http.
async function startProxyEdition() {
    const createDdbsh = (await import("./ddbsh.js")).default;

    // One-time handshake: mint a session + per-session token (and learn the
    // proxy version for SHOW CONNECTION, D-P6f-9).
    const sess = await proxyPost("session", { headers: { "content-type": "application/json" } });
    if (sess.status !== 200) {
        throw new Error(`proxy /session HTTP ${sess.status}: ${sess.text}`);
    }
    const { session: sessionId, token, version } = JSON.parse(sess.text);
    if (!sessionId || !token) {
        throw new Error("proxy /session returned no session/token");
    }
    const authHeaders = { "x-ddbsh-session": sessionId, "x-ddbsh-token": token };

    const Module = await createDdbsh();
    if (version) {
        Module.ddbshProxyVersion = version; // surfaced by SHOW CONNECTION.
    }

    // CONNECT control request: the engine's CONNECT statement fires this through
    // FetchTransport::on_connect. For an endpoint override (e.g. a local
    // ExtendDB) attach the per-endpoint profile + CA bundle from the page URL.
    Module.connectDdb = async (region, endpoint) => {
        const payload = { region };
        if (endpoint) {
            payload.endpoint = endpoint;
            if (endpointConfig.profile) {
                payload.profile = endpointConfig.profile;
            }
            if (endpointConfig.caBundle) {
                payload.ca_bundle = endpointConfig.caBundle;
            }
        }
        const r = await proxyPost("connect", {
            headers: { "content-type": "application/json", ...authHeaders },
            body: JSON.stringify(payload),
        });
        if (r.status === 200) {
            return ""; // success: empty string per the on_connect contract.
        }
        try {
            return JSON.parse(r.text).message || `connect HTTP ${r.status}`;
        } catch {
            return `connect HTTP ${r.status}`;
        }
    };

    // Data-plane forward. A 2xx returns the body verbatim; a service error
    // (a `__type` document) is returned to the codec to map; a proxy/transport
    // error throws (FetchTransport renders it as a transport failure).
    Module.fetchDdb = async (op, body) => {
        const r = await proxyPost("ddb", {
            headers: {
                "content-type": "application/x-amz-json-1.0",
                "x-ddbsh-op": op,
                ...authHeaders,
            },
            body,
        });
        if (r.status >= 200 && r.status < 300) {
            return r.text;
        }
        try {
            if (JSON.parse(r.text).__type) {
                return r.text; // service error -> codec maps it.
            }
        } catch {
            // fall through to a transport-level failure.
        }
        throw new Error(`proxy /ddb HTTP ${r.status}: ${r.text.slice(0, 200)}`);
    };

    // Bootstrap the default-region AWS target so the first data-plane call works
    // without an explicit CONNECT (the glue "establishes the proxy connection
    // out of band at bootstrap", per FetchTransport::on_connect). Non-fatal: a
    // failure (e.g. unresolved credentials) is reported, and the user can fix
    // credentials and CONNECT.
    const connectMsg = await Module.connectDdb(REGION, "");
    if (connectMsg) {
        appendEntries([{ text: `proxy connect: ${connectMsg}\n`, kind: "stderr" }]);
    }

    session = new Module.DdbshSession(REGION);
    backendName = "proxy → real backend";
    setReady();
}

// Bring up the in-tab (in-browser ExtendDB) edition — no server, no network, no
// credentials. The adapter is a drop-in (D-P6f-8), imported dynamically so the
// proxy edition never depends on it.
async function startInTabEdition() {
    const createDdbsh = (await import("./ddbsh-extenddb.js")).default;
    const extenddb = await import("./vendor/extenddb/extenddb_wasm_adapter.js");
    const initExtendDb = extenddb.default;
    const { ExtendDb } = extenddb;

    await initExtendDb();                 // browser: fetch sibling _bg.wasm over HTTP.
    const db = await ExtendDb.create();   // async migrations (in-memory SQLite).
    const Module = await createDdbsh();   // ddbsh-extenddb.wasm up; embind registered.
    Module.submit = (op, body) => db.submit(op, body); // wire the Asyncify bridge.

    session = new Module.DdbshSession(REGION);
    backendName = "ExtendDB (in-browser)";
    setReady();
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

    // Bind events now; controls stay disabled until the module is ready.
    els.runBtn.addEventListener("click", runFromEditor);
    els.clearBtn.addEventListener("click", onClear);
    els.editor.addEventListener("keydown", onEditorKeydown);
    els.editor.addEventListener("input", onEditorInput);
    els.fileInput.addEventListener("change", onFileChosen);

    try {
        if (await proxyPresent()) {
            await startProxyEdition();
        } else {
            await startInTabEdition();
        }
    } catch (e) {
        els.backendLabel.textContent = "failed to load";
        els.backendDot.className = "dot";
        appendEntries([{ text: `failed to initialize ddbsh: ${e && e.message ? e.message : e}\n`,
                         kind: "stderr" }]);
    }
}

start();
