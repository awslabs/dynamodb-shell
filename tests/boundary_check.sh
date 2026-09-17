#!/usr/bin/env bash
# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# boundary_check.sh — enforce the library/CLI boundary (tenet 3, §14 item 6).
#
# The engine library (include/ddbsh/ + src/) must never touch a terminal,
# process globals, or perform its own I/O: no <iostream>, cout/cerr,
# printf/fprintf/fputs, FILE*, or exit()/abort(). All output flows out as data
# (ExecOutcome) and is formatted by the Renderer at the edge; the CLI writes
# bytes. This check keeps the boundary from silently rotting.
#
# Comments and string literals are stripped before matching so that prose
# describing the rule (e.g. "no FILE*, no exit()") does not trip it. Generated
# flex/bison output lives in the build tree and is not scanned; the hand-written
# lexer.l / grammar.yy ARE scanned.
#
# Usage: boundary_check.sh <repo-root>
# Exits nonzero (with the offending file:line) on any real violation.

set -euo pipefail

ROOT="${1:?usage: boundary_check.sh <repo-root>}"
SCAN_DIRS=("$ROOT/include/ddbsh" "$ROOT/src")

PATTERNS=(
    '#include[[:space:]]*<iostream>'
    '\bstd::cout\b'
    '\bstd::cerr\b'
    '\bprintf\b'
    '\bfprintf\b'
    '\bfputs\b'
    '\bFILE[[:space:]]*\*'
    '\bexit[[:space:]]*\('
    '\babort[[:space:]]*\('
)

# Strip // line comments, /* ... */ block comments, and "..." string literals
# from a file, preserving line numbers (blank out matched spans). Uses a small
# awk state machine sufficient for our sources.
strip_code() {
    awk '
    BEGIN { in_block = 0 }
    {
        line = $0
        out = ""
        i = 1
        n = length(line)
        while (i <= n) {
            c = substr(line, i, 1)
            two = substr(line, i, 2)
            if (in_block) {
                if (two == "*/") { in_block = 0; i += 2 } else { i += 1 }
                continue
            }
            if (two == "/*") { in_block = 1; i += 2; continue }
            if (two == "//") { break }              # rest of line is a comment
            if (c == "\"") {                          # skip a string literal
                i += 1
                while (i <= n) {
                    d = substr(line, i, 1)
                    if (d == "\\") { i += 2; continue }
                    if (d == "\"") { i += 1; break }
                    i += 1
                }
                continue
            }
            out = out c
            i += 1
        }
        print out
    }' "$1"
}

status=0
mapfile -t FILES < <(find "${SCAN_DIRS[@]}" \
    \( -name '*.hpp' -o -name '*.cpp' -o -name '*.l' -o -name '*.yy' \) -type f 2>/dev/null)

for f in "${FILES[@]}"; do
    stripped="$(strip_code "$f")"
    for pat in "${PATTERNS[@]}"; do
        if hits="$(printf '%s\n' "$stripped" | grep -nE "$pat" 2>/dev/null)"; then
            echo "BOUNDARY VIOLATION: forbidden pattern /$pat/ in $f:" >&2
            printf '%s\n' "$hits" | sed "s#^#  $f:#" >&2
            status=1
        fi
    done
done

if [ "$status" -eq 0 ]; then
    echo "engine boundary check: clean (no terminal/I/O/exit in include/ddbsh + src)"
fi
exit "$status"
