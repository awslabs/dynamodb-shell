#!/usr/bin/env bash
# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# golden_absence_check.sh — enforce D19 (P4d embedding API).
#
# The embedding API (ddbsh/connection.hpp + ddbsh/aws/database.hpp) introduces a
# handful of error messages that exist ONLY on its own misuse paths — a closed
# handle, a query() kind mismatch, a bad parameter bind. The CLI never drives
# those paths (it uses the engine SPI directly), so none of these strings can
# ever appear in a golden .exp file. This gate asserts that property mechanically
# so a future change that accidentally routes an embedding-API string into the
# CLI's observable output (and thus a golden) is caught, keeping the 705/705 CLI
# fidelity contract and the embedding-API-only guarantee honest.
#
# Usage: golden_absence_check.sh <repo-root>
# Exits nonzero (listing the offending file:line) if any embedding-API-only
# string is found under the golden suite.

set -euo pipefail

ROOT="${1:?usage: golden_absence_check.sh <repo-root>}"
GOLDEN_DIR="$ROOT/tests/golden"

if [ ! -d "$GOLDEN_DIR" ]; then
    echo "golden_absence_check: expected golden directory not found: $GOLDEN_DIR" >&2
    exit 2
fi

# Fixed (non-regex) substrings of the embedding-API-only messages emitted only
# by ddbsh/connection.hpp + ddbsh/aws/database.hpp (grep -F, so '(' / '#' / ':'
# are literal). Keep in sync with the invalid_argument messages in
# src/connection/connection.cpp.
NEEDLES=(
    'connection is closed'
    'query() requires a row-returning statement'
    'positional placeholder #'
    'bound positional parameter #'
    'has no matching placeholder'
    'cannot bind a DynamoDB binary or set value'
    'cannot bind value of unknown type'
    'positional bind index must be >= 1'
)

status=0
for needle in "${NEEDLES[@]}"; do
    if hits="$(grep -rnF -- "$needle" "$GOLDEN_DIR" 2>/dev/null)"; then
        echo "GOLDEN VIOLATION: embedding-API-only string \"$needle\" found in the golden suite:" >&2
        printf '%s\n' "$hits" >&2
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "golden absence check: clean (no embedding-API-only error string appears in any golden .exp)"
fi
exit "$status"
