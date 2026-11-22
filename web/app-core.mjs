// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// app-core.mjs — the DOM-free logic behind the P6e browser UI (D-P6e-3).
//
// Everything in this module is pure JavaScript with no dependency on the DOM,
// the browser, `localStorage`, or the ddbsh WASM module, so it is unit-testable
// under Node (see app-core.test.mjs). The thin DOM wiring that consumes it lives
// in app.js and is covered by the human hand-test.
//
// What lives here:
//   - buildPrompt:            the shell prompt string (mirrors the CLI prompt).
//   - buildTranscriptEntries: a RunResult + echoed input -> ordered render blocks.
//   - parseCredentials / validateCredentials / credentialStatus /
//     maskAccessKeyId:        short-term credential handling (P9, D-P9-3).
//   - credentialEnvironmentProblem: whether this window may take credentials
//                             at all (secure context, not framed, not opened
//                             by another page's script; P10, G1).
//   - fileToSubmission:       file text -> one submission (the `-f`/source path).
//   - createHistory:          the command-history ring (push/cap/dedup/traverse/clear).
//   - historyRemovedElsewhere: whether a `storage` event means another tab
//                             deleted the saved history (P10).
//   - createRunGuard:         the one-submission-in-flight re-entrancy guard.
//   - versionLabel:           the header's "version <v>" text, read from the
//                             engine the page loaded (P10, D-P10-14).
//   - busyText / createBusyClock: the header's "running… <N>s" indicator and
//                             its start/finish announcements (P10, D-P10-16).
//
// No engine logic crosses into JS (tenet 3): the faithful shell text is produced
// by the engine's Renderer and arrives verbatim in RunResult.segments; this
// module only arranges, prefixes, and bounds it for presentation.

/**
 * Build the shell prompt string shown in front of each echoed input line.
 *
 * Mirrors the original ddbsh prompt (architecture reference §4, checklist #11):
 * the region, an optional `(*)` marker when the endpoint is overridden, then
 * `> `. The original reads input a line at a time and shows the same prompt for
 * every line, so there is no distinct continuation prompt; `continuation` is
 * accepted for symmetry with a future distinct-continuation prompt but currently
 * yields the same string, preserving fidelity.
 *
 * @param {string} region            the active region (e.g. "us-east-1").
 * @param {boolean} endpointOverridden  true when an endpoint override is set.
 * @param {boolean} [continuation]   reserved; the original has no distinct
 *                                   continuation prompt, so this is ignored.
 * @returns {string} the prompt, e.g. "us-east-1> " or "us-east-1(*)> ".
 */
export function buildPrompt(region, endpointOverridden, continuation = false) {
    void continuation; // no distinct continuation prompt in the original (see doc).
    const marker = endpointOverridden ? "(*)" : "";
    return `${region}${marker}> `;
}

/**
 * Turn one submission (the echoed input plus its RunResult) into the ordered
 * list of transcript blocks the UI appends to the result pane.
 *
 * Produces, in order:
 *   1. a `prompt` block: every line of `input` prefixed with `prompt`
 *      (so a multi-line statement reads like it would in the CLI);
 *   2. the result text in EMISSION order: one `stdout` / `stderr` block per
 *      entry of RunResult.segments (P9, D-P9-02-11), so an error between two
 *      results appears between them, as it does in the CLI. A RunResult
 *      without `segments` falls back to a `stdout` block (RunResult.stdout)
 *      followed by a `stderr` block (RunResult.stderr).
 * Empty strings produce no block (a quiet success appends only the prompt echo;
 * a blank submission appends nothing).
 *
 * The block text is returned raw; the caller MUST insert it via `textContent`
 * (never innerHTML) so item data is never interpreted as HTML (D-P6e-6).
 *
 * @param {string} prompt    the prompt string from buildPrompt.
 * @param {string} input     the raw submission text (may be multi-line).
 * @param {{stdout?: string, stderr?: string,
 *          segments?: Array<{stream: string, text: string}>}} runResult
 *        the engine result.
 * @returns {Array<{text: string, kind: "prompt"|"stdout"|"stderr"}>}
 */
export function buildTranscriptEntries(prompt, input, runResult) {
    const entries = [];
    const result = runResult || {};

    if (typeof input === "string" && input.length > 0) {
        const echoed = input
            .split("\n")
            .map((line) => `${prompt}${line}`)
            .join("\n");
        entries.push({ text: echoed, kind: "prompt" });
    }

    const segments = segmentList(result.segments);
    if (segments !== null) {
        for (const seg of segments) {
            const text = seg && typeof seg.text === "string" ? seg.text : "";
            if (text.length === 0) {
                continue;
            }
            const kind = seg.stream === "stderr" ? "stderr" : "stdout";
            const last = entries[entries.length - 1];
            if (last && last.kind === kind) {
                last.text += text; // coalesce adjacent same-stream segments.
            } else {
                entries.push({ text, kind });
            }
        }
        return entries;
    }

    const out = typeof result.stdout === "string" ? result.stdout : "";
    if (out.length > 0) {
        entries.push({ text: out, kind: "stdout" });
    }

    const err = typeof result.stderr === "string" ? result.stderr : "";
    if (err.length > 0) {
        entries.push({ text: err, kind: "stderr" });
    }

    return entries;
}

// Normalize RunResult.segments to a plain array, or null when absent. Embind
// returns a registered std::vector as a handle with size()/get(i); a plain
// array (tests, or a caller that already converted it) is used as-is.
function segmentList(segments) {
    if (Array.isArray(segments)) {
        return segments;
    }
    if (segments && typeof segments.size === "function" && typeof segments.get === "function") {
        const n = segments.size();
        const list = [];
        for (let i = 0; i < n; i += 1) {
            list.push(segments.get(i));
        }
        return list;
    }
    return null;
}

// ---- short-term credentials (P9, D-P9-3 / D-P9-4) --------------------------

const LONG_TERM_KEY = /^AKIA/;
const SHORT_TERM_KEY = /^ASIA[A-Z0-9]{12,124}$/;

/**
 * Show an access key id without revealing it: the 4-character prefix and the
 * last four characters (e.g. "ASIA…WXYZ"). Never used on a secret or token.
 *
 * @param {string} accessKeyId
 * @returns {string}
 */
export function maskAccessKeyId(accessKeyId) {
    const id = typeof accessKeyId === "string" ? accessKeyId : "";
    if (id.length <= 8) {
        return "…";
    }
    return `${id.slice(0, 4)}…${id.slice(-4)}`;
}

/**
 * Parse pasted credential text into a credential set. Accepts the outputs of
 * the three commands the page recommends:
 *   1. STS JSON — `{"Credentials": {"AccessKeyId", "SecretAccessKey",
 *      "SessionToken", "Expiration"}, ...}` (get-session-token, assume-role);
 *   2. credential-process JSON — `{"Version": 1, "AccessKeyId", ...}`
 *      (`aws configure export-credentials --format process`);
 *   3. environment lines — `[export ]AWS_ACCESS_KEY_ID=…` (bash, `--format
 *      env` / `env-no-export`), `$Env:AWS_ACCESS_KEY_ID="…"` (PowerShell) or
 *      `set AWS_ACCESS_KEY_ID=…` (cmd); values optionally quoted.
 * The result is then validated with validateCredentials. No message ever
 * contains the secret or the token; the key id appears only masked.
 *
 * @param {string} text  the pasted text.
 * @param {number} [nowMs]  the current time (ms since epoch), for expiry.
 * @returns {{ok: true, credentials: {accessKeyId: string, secretAccessKey: string,
 *            sessionToken: string, expiration?: Date}} | {ok: false, message: string}}
 */
export function parseCredentials(text, nowMs = Date.now()) {
    if (typeof text !== "string" || text.trim() === "") {
        return { ok: false, message: "no credentials were pasted" };
    }
    const trimmed = text.trim();
    let raw = null;
    if (trimmed.startsWith("{")) {
        let obj;
        try {
            obj = JSON.parse(trimmed);
        } catch {
            return { ok: false, message: "the pasted text looks like JSON but does not parse" };
        }
        const src = obj && typeof obj.Credentials === "object" && obj.Credentials !== null
            ? obj.Credentials : obj;
        raw = {
            accessKeyId: src.AccessKeyId,
            secretAccessKey: src.SecretAccessKey,
            sessionToken: src.SessionToken,
            expiration: src.Expiration,
        };
    } else {
        raw = {};
        const names = {
            AWS_ACCESS_KEY_ID: "accessKeyId",
            AWS_SECRET_ACCESS_KEY: "secretAccessKey",
            AWS_SESSION_TOKEN: "sessionToken",
            AWS_CREDENTIAL_EXPIRATION: "expiration",
        };
        const lineRe = /^(?:export\s+|set\s+|\$Env:)?(AWS_[A-Z_]+)\s*=\s*(.*)$/i;
        for (const line of trimmed.split(/\r?\n/)) {
            const l = line.trim();
            if (l === "" || l.startsWith("#")) {
                continue;
            }
            const m = l.match(lineRe);
            if (!m) {
                continue;
            }
            const field = names[m[1].toUpperCase()];
            if (!field) {
                continue;
            }
            let value = m[2].trim();
            if (value.length >= 2 && (value[0] === '"' || value[0] === "'") &&
                value[value.length - 1] === value[0]) {
                value = value.slice(1, -1);
            }
            raw[field] = value;
        }
        if (Object.keys(raw).length === 0) {
            return { ok: false, message: "no AWS credentials found in the pasted text" };
        }
    }
    const credentials = {
        accessKeyId: typeof raw.accessKeyId === "string" ? raw.accessKeyId.trim() : "",
        secretAccessKey: typeof raw.secretAccessKey === "string" ? raw.secretAccessKey : "",
        sessionToken: typeof raw.sessionToken === "string" ? raw.sessionToken.trim() : "",
    };
    if (raw.expiration !== undefined && raw.expiration !== null && raw.expiration !== "") {
        const d = raw.expiration instanceof Date ? raw.expiration : new Date(raw.expiration);
        credentials.expiration = d;
    }
    const problem = validateCredentials(credentials, nowMs);
    if (problem !== null) {
        return { ok: false, message: problem };
    }
    return { ok: true, credentials };
}

/**
 * Check a credential set against the browser edition's rules (D-P9-3): an
 * `ASIA…` short-term access key id, a secret, a session token, and (when
 * given) an expiration in the future.
 *
 * @param {{accessKeyId?: string, secretAccessKey?: string, sessionToken?: string,
 *          expiration?: Date}} c
 * @param {number} [nowMs]
 * @returns {string|null} null when acceptable, else the reason.
 */
export function validateCredentials(c, nowMs = Date.now()) {
    const cred = c || {};
    const id = typeof cred.accessKeyId === "string" ? cred.accessKeyId : "";
    if (id === "") {
        return "no access key id found";
    }
    if (LONG_TERM_KEY.test(id)) {
        return "long-term access keys (AKIA…) are not accepted; use one of the commands " +
            "below to get short-term credentials";
    }
    if (!SHORT_TERM_KEY.test(id)) {
        return "not an AWS short-term access key id";
    }
    const secret = typeof cred.secretAccessKey === "string" ? cred.secretAccessKey : "";
    if (secret === "" || /\s/.test(secret)) {
        return "the secret access key is missing or malformed";
    }
    const token = typeof cred.sessionToken === "string" ? cred.sessionToken : "";
    if (token === "") {
        return "a session token is required (short-term credentials only)";
    }
    if (cred.expiration !== undefined) {
        if (!(cred.expiration instanceof Date) || Number.isNaN(cred.expiration.getTime())) {
            return "the credential expiration is not a valid date";
        }
        if (cred.expiration.getTime() <= nowMs) {
            return `these credentials expired at ${cred.expiration.toISOString()}`;
        }
    }
    return null;
}

/**
 * The credentials status line: "none", "ASIA…WXYZ, expires 23:03Z (in 58 min)",
 * "ASIA…WXYZ (expiry unknown)" or "expired".
 *
 * @param {{accessKeyId: string, expiration?: Date} | null} c
 * @param {number} [nowMs]
 * @returns {string}
 */
export function credentialStatus(c, nowMs = Date.now()) {
    if (!c) {
        return "none";
    }
    const masked = maskAccessKeyId(c.accessKeyId);
    if (!(c.expiration instanceof Date)) {
        return `${masked} (expiry unknown)`;
    }
    const left = c.expiration.getTime() - nowMs;
    if (left <= 0) {
        return "expired";
    }
    const hhmm = c.expiration.toISOString().slice(11, 16);
    const minutes = Math.floor(left / 60000);
    return `${masked}, expires ${hhmm}Z (in ${minutes} min)`;
}

/**
 * Decide whether this window may accept credentials at all (P10, G1). The
 * page refuses them, before anything is pasted, when:
 *   - it is inside a frame (`window.top !== window.self`): the framing page may
 *     overlay or restyle it to capture what is typed (clickjacking). A host
 *     sends `frame-ancestors 'none'` to stop framing; this check also covers a
 *     host that cannot send that header;
 *   - another page's script opened it (`window.opener` is set): a same-origin
 *     opener has full access to this window's DOM, including the credentials
 *     text box;
 *   - it is not in a secure context (not https:// or http://localhost): the
 *     page could have been modified in transit.
 * The caller gathers the three facts from `window` (see app.js); this function
 * only decides, so the rules are unit-testable without a DOM.
 *
 * @param {{secureContext: boolean, framed: boolean, opened: boolean}} env
 * @returns {string|null} null when credentials may be used, else the reason
 *          to show (which also says what to do instead).
 */
export function credentialEnvironmentProblem({ secureContext, framed, opened } = {}) {
    if (framed !== false) {
        return "credentials are refused: this page is shown inside a frame on another page, " +
            "which could capture what you type; open the page directly in its own tab";
    }
    if (opened !== false) {
        return "credentials are refused: another page's script opened this window and can reach " +
            "into it; open the page directly by typing or pasting its address";
    }
    if (secureContext !== true) {
        return "credentials can only be used on https:// or http://localhost; " +
            "serve over HTTPS or use an SSH tunnel";
    }
    return null;
}

/**
 * Convert uploaded file text into one submission (the browser equivalent of the
 * CLI's `-f` / `source`). The whole file runs as a single submission; the engine
 * splits the `;`-terminated statements. A trailing newline is harmless (the
 * lexer tolerates it) but is trimmed so the echoed block does not end in a blank
 * prompt line.
 *
 * @param {string} text  the file contents.
 * @returns {string} the submission text.
 */
export function fileToSubmission(text) {
    if (typeof text !== "string") {
        return "";
    }
    return text.replace(/\n+$/, "");
}

/**
 * Create a bounded, de-duplicating command-history ring with a recall cursor.
 *
 * Behavior:
 *   - push(entry): stores non-blank entries; skips an entry identical to the
 *     most recent one (adjacent de-dup); caps the ring at `limit` (oldest
 *     dropped); resets the recall cursor to the live draft.
 *   - previous(): walks toward older entries (ArrowUp); clamps at the oldest.
 *   - next(): walks toward newer entries (ArrowDown); returns null past the
 *     newest (the live draft).
 *   - list(): a copy of the entries, oldest first (for persistence).
 *   - clear(): drops every entry and resets the recall cursor (the page's
 *     "Clear history" button; P10, D-P10-6).
 *
 * The caller owns persistence (e.g. localStorage): seed via `entries` and write
 * `list()` back after each push (and remove the stored copy on clear()). This
 * module touches no storage.
 *
 * @param {{limit?: number, entries?: string[]}} [opts]
 */
export function createHistory({ limit = 500, entries = [] } = {}) {
    const cap = Math.max(1, limit | 0);
    const items = (Array.isArray(entries) ? entries : [])
        .filter((e) => typeof e === "string" && e.trim() !== "")
        .slice(-cap);
    let cursor = items.length; // points at the live draft (one past the newest).

    return {
        /** Store a command; returns true if it was added to the ring. */
        push(entry) {
            if (typeof entry !== "string" || entry.trim() === "") {
                cursor = items.length;
                return false;
            }
            const added = items.length === 0 || items[items.length - 1] !== entry;
            if (added) {
                items.push(entry);
                while (items.length > cap) {
                    items.shift();
                }
            }
            cursor = items.length;
            return added;
        },

        /** Move the recall cursor to an older entry (ArrowUp); null if empty. */
        previous() {
            if (items.length === 0) {
                return null;
            }
            if (cursor > 0) {
                cursor -= 1;
            }
            return items[cursor];
        },

        /** Move toward newer entries (ArrowDown); null at the live draft. */
        next() {
            if (cursor < items.length) {
                cursor += 1;
            }
            if (cursor >= items.length) {
                return null;
            }
            return items[cursor];
        },

        /** Reset the recall cursor to the live draft. */
        resetCursor() {
            cursor = items.length;
        },

        /** A copy of the stored entries, oldest first. */
        list() {
            return items.slice();
        },

        /** Drop every entry and reset the recall cursor to the (empty) draft. */
        clear() {
            items.length = 0;
            cursor = 0;
        },

        /** The number of stored entries. */
        size() {
            return items.length;
        },
    };
}

/**
 * Decide whether a `storage` event means another tab of this page deleted the
 * saved command history (P10): its "Clear history" button removed the key, or
 * the origin's whole storage area was cleared (the event's `key` is null then).
 * The tab that receives such an event forgets its in-memory copy too; otherwise
 * its next command would write the deleted statements back. Browsers deliver
 * storage events only to the origin's other documents, never to the one that
 * made the change.
 *
 * @param {{key?: string|null, newValue?: string|null}} event  the StorageEvent
 *        (only `key` and `newValue` are read).
 * @param {string} historyKey  the storage key of the saved history.
 * @returns {boolean} true when the saved history is gone.
 */
export function historyRemovedElsewhere({ key, newValue } = {}, historyKey) {
    if (key === null) {
        return true; // the whole storage area was cleared.
    }
    return typeof historyKey === "string" && key === historyKey && newValue === null;
}

/**
 * Create the history-recall state machine that ArrowUp/ArrowDown drive.
 *
 * This owns the one piece of recall logic that is easy to get wrong (and did go
 * wrong before it was centralized here): the interaction between the live draft
 * the user was typing and the history cursor. It is DOM-free so the exact edges
 * are unit-testable; app.js only maps key events and the editor value onto it.
 *
 * Both `up()` and `down()` return either a string the caller MUST write into the
 * editor (and should `preventDefault`), or `null` meaning "leave the buffer
 * alone, let the arrow key behave normally". The rules:
 *   - up(current): recall an older entry; the first recall stashes `current` as
 *     the draft to restore later. Returns null (no-op) when there is nothing
 *     older (empty history / already at the oldest-but-unchanged position).
 *   - down(): while recalling, walk toward newer entries; past the newest,
 *     restore the stashed draft and end the recall. Returns null (no-op) when
 *     not recalling, so a plain Down at the live line never destroys typed
 *     input.
 *   - reset(): abandon any recall (call on a manual edit or after a submit).
 *
 * @param {ReturnType<typeof createHistory>} history  the history ring.
 */
export function createHistoryRecall(history) {
    let draft = null; // the live buffer stashed while recalling; null = not recalling.

    return {
        /** True while a recall is in progress (a draft is stashed). */
        get recalling() {
            return draft !== null;
        },

        /**
         * ArrowUp: recall an older entry.
         * @param {string} current  the current editor value (stashed on first recall).
         * @returns {string|null} the value to set, or null for "leave alone".
         */
        up(current) {
            const entry = history.previous();
            if (entry === null) {
                return null; // nothing older: no-op, let the key behave normally.
            }
            if (draft === null) {
                draft = typeof current === "string" ? current : "";
            }
            return entry;
        },

        /**
         * ArrowDown: walk toward newer entries, restoring the draft past the newest.
         * @returns {string|null} the value to set, or null for "leave alone".
         */
        down() {
            if (draft === null) {
                return null; // not recalling: no-op, don't wipe live input.
            }
            const entry = history.next();
            if (entry !== null) {
                return entry;
            }
            const restored = draft;
            draft = null; // back at the live draft: recall ends.
            return restored;
        },

        /** Abandon any in-progress recall (manual edit, or after a submit). */
        reset() {
            draft = null;
            history.resetCursor();
        },
    };
}

/**
 * Create a single-flight guard so only one submission runs at a time (Asyncify
 * re-entrancy safety, D-P6e-2). `begin()` returns false when a submission is
 * already in flight; the caller must pair a successful `begin()` with `end()`.
 */
export function createRunGuard() {
    let inFlight = false;
    return {
        /** True while a submission is in flight. */
        get busy() {
            return inFlight;
        },
        /** Claim the single slot; false if already busy. */
        begin() {
            if (inFlight) {
                return false;
            }
            inFlight = true;
            return true;
        },
        /** Release the slot. */
        end() {
            inFlight = false;
        },
    };
}

// ---- page header (P10, D-P10-14 / D-P10-16) ---------------------------------

// The engine's version strings: CMake project(VERSION) allows only numeric
// components, and ddbsh::version() prints two to four of them ("0.12.1").
const ENGINE_VERSION = /^[0-9]+(\.[0-9]+){1,3}$/;

/**
 * The version text for the header title, read from the engine the page loaded
 * (D-P10-14): "version 0.12.1", so the title reads "ddbsh (web) version
 * 0.12.1". Returns "" (the title stays "ddbsh (web)") when the module has no
 * `version()` export, which happens when a new app.js meets an older
 * ddbsh.wasm during an upload, or when the export throws or returns something
 * that is not a version. The page works either way.
 *
 * @param {{version?: () => string} | null} module  the instantiated WASM module.
 * @returns {string}
 */
export function versionLabel(module) {
    let version;
    try {
        version = module && typeof module.version === "function" ? module.version() : undefined;
    } catch {
        return "";
    }
    return typeof version === "string" && ENGINE_VERSION.test(version) ? `version ${version}` : "";
}

/**
 * The whole seconds elapsed between two times (ms), never negative.
 *
 * @param {number} startMs
 * @param {number} nowMs
 * @returns {number}
 */
export function elapsedSeconds(startMs, nowMs) {
    return Math.max(0, Math.floor((nowMs - startMs) / 1000));
}

/**
 * The busy indicator's visible text while a submission runs (D-P10-16):
 * "running… 0s", "running… 1s", …
 *
 * @param {number} seconds  whole seconds elapsed.
 * @returns {string}
 */
export function busyText(seconds) {
    return `running… ${seconds}s`;
}

/**
 * What a screen reader hears when a submission finishes (D-P10-16). The start
 * is announced as "command started"; the seconds are never announced as they
 * count.
 *
 * @param {number} seconds  whole seconds the submission took.
 * @returns {string}
 */
export function busyFinishedText(seconds) {
    return `command finished after ${seconds} ${seconds === 1 ? "second" : "seconds"}`;
}

/**
 * Create the clock behind the header's busy indicator (D-P10-16). It is
 * DOM-free: the caller supplies the clock, the frame scheduler and two output
 * callbacks, so the start/stop behavior is unit-testable with a fake clock.
 *
 *   - start(): shows "running… 0s", announces "command started", then checks
 *     the elapsed time on every animation frame and re-renders when the whole
 *     seconds change. A second start() while running changes nothing.
 *   - stop(): cancels the pending frame, hides the indicator (render(null))
 *     and announces how long the submission took. stop() while idle changes
 *     nothing.
 *
 * Why animation frames rather than a one-second timer: in headless Chromium
 * 159 a setTimeout due at a whole second sometimes fired about a second late
 * while a CREATE TABLE waited on DynamoDB, with no long task on the main
 * thread, so the count skipped a second (P10 developer round). A frame
 * callback runs on every frame while the page is visible and costs one
 * comparison; nothing is rendered between whole seconds. In a hidden tab no
 * frames run and nothing needs drawing; the first frame after the tab shows
 * again renders the true elapsed seconds.
 *
 * @param {{now: () => number,
 *          requestFrame: (fn: () => void) => unknown,
 *          cancelFrame: (handle: unknown) => void,
 *          render: (text: string | null) => void,
 *          announce: (text: string) => void}} io
 *        `now` returns milliseconds from a monotonic clock (performance.now
 *        in the page); `requestFrame`/`cancelFrame` are
 *        requestAnimationFrame/cancelAnimationFrame; `render(null)` hides the
 *        indicator.
 */
export function createBusyClock({ now, requestFrame, cancelFrame, render, announce }) {
    let startMs = null; // null while idle.
    let shownSeconds = -1; // the seconds on screen; -1 while idle.
    let frame = null;

    function onFrame() {
        frame = null;
        if (startMs === null) {
            return; // stopped while this frame was pending.
        }
        const seconds = elapsedSeconds(startMs, now());
        if (seconds !== shownSeconds) {
            shownSeconds = seconds;
            render(busyText(seconds));
        }
        frame = requestFrame(onFrame);
    }

    return {
        /** True while a submission is being timed. */
        get running() {
            return startMs !== null;
        },

        /** Show the indicator and start counting; false if already running. */
        start() {
            if (startMs !== null) {
                return false;
            }
            startMs = now();
            shownSeconds = 0;
            render(busyText(0));
            announce("command started");
            frame = requestFrame(onFrame);
            return true;
        },

        /** Hide the indicator and announce the finish; false if idle. */
        stop() {
            if (startMs === null) {
                return false;
            }
            if (frame !== null) {
                cancelFrame(frame);
                frame = null;
            }
            const seconds = elapsedSeconds(startMs, now());
            startMs = null;
            shownSeconds = -1;
            render(null);
            announce(busyFinishedText(seconds));
            return true;
        },
    };
}
