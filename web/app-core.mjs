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
//   - fileToSubmission:       file text -> one submission (the `-f`/source path).
//   - createHistory:          the command-history ring (push/cap/dedup/traverse).
//   - createRunGuard:         the one-submission-in-flight re-entrancy guard.
//
// No engine logic crosses into JS (tenet 3): the faithful shell text is produced
// by the engine's Renderer and arrives verbatim in RunResult.stdout/stderr; this
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
 *   2. a `stdout` block: RunResult.stdout verbatim (the faithful shell text);
 *   3. a `stderr` block: RunResult.stderr verbatim (diagnostics/errors).
 * Empty strings produce no block (a quiet success appends only the prompt echo;
 * a blank submission appends nothing).
 *
 * The block text is returned raw; the caller MUST insert it via `textContent`
 * (never innerHTML) so item data is never interpreted as HTML (D-P6e-6).
 *
 * @param {string} prompt    the prompt string from buildPrompt.
 * @param {string} input     the raw submission text (may be multi-line).
 * @param {{stdout?: string, stderr?: string}} runResult  the engine result.
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
 *
 * The caller owns persistence (e.g. localStorage): seed via `entries` and write
 * `list()` back after each push. This module touches no storage.
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

        /** The number of stored entries. */
        size() {
            return items.length;
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
