#!/usr/bin/env bash
# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# run_embed_test.sh — verify the library-embedding sample as an EXTERNAL
# consumer (2h-4). It:
#   1. installs the just-built engine library to a throwaway prefix;
#   2. builds examples/embed_demo via its STANDALONE Makefile (not the project
#      CMake) against that prefix — proving the `-lddbsh` consumer path;
#   3. runs the (offline, in-memory) demo and diffs its stdout against the
#      committed golden examples/embed_demo.expected.txt.
#
# It needs no AWS account (the sample supplies its own in-memory IDDBClient) and
# is deterministic, but it DOES need an install step + a second compiler
# invocation, so it is gated behind the CMake option DDBSH_TEST_EMBED_SAMPLE
# (OFF by default) — the normal unit build is unaffected.
#
# Args (passed by the CTest registration):
#   $1 = cmake executable
#   $2 = the build dir to install from (CMAKE_BINARY_DIR)
#   $3 = the examples source dir
#   $4 = the C++ compiler to build the sample with
#   $5 = the expected-output golden file

set -euo pipefail

CMAKE="$1"
BUILD_DIR="$2"
EXAMPLES_DIR="$3"
SAMPLE_CXX="$4"
EXPECTED="$5"

PREFIX="$(mktemp -d)"
WORK="$(mktemp -d)"
cleanup() { rm -rf "$PREFIX" "$WORK"; }
trap cleanup EXIT

echo "=== installing libddbsh to $PREFIX ==="
"$CMAKE" --install "$BUILD_DIR" --prefix "$PREFIX"

echo "=== building embed_demo via its standalone Makefile ==="
cp "$EXAMPLES_DIR/embed_demo.cpp" "$EXAMPLES_DIR/Makefile" "$WORK/"
make -C "$WORK" DDBSH_PREFIX="$PREFIX" CXX="$SAMPLE_CXX"

echo "=== running embed_demo and diffing against the golden ==="
"$WORK/embed_demo" > "$WORK/out.txt"
if diff -u "$EXPECTED" "$WORK/out.txt"; then
    echo "embed sample: PASS"
else
    echo "embed sample: FAIL (stdout differs from $EXPECTED)" >&2
    exit 1
fi
