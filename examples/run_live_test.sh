#!/usr/bin/env bash
# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# run_live_test.sh — verify the examples/live_demo sample as an EXTERNAL
# consumer of the installed find_package(ddbsh) package, against REAL DynamoDB
# (D8). It:
#   1. installs the engine library + the ddbsh::aws component to a throwaway
#      prefix;
#   2. configures + builds examples/live_demo via its OWN CMakeLists using
#      find_package(ddbsh) + find_package(AWSSDK) pointed at that prefix (plus
#      the AWS SDK prefix) — proving the real find_package(ddbsh) consumer path;
#   3. runs the demo against real DynamoDB us-east-1 via the standard ~/.aws
#      chain and asserts it succeeds end to end (CREATE -> INSERT -> the three
#      SELECT plans -> DROP, exit 0, table self-cleaned).
#
# This is a LIVENESS / end-to-end smoke test, NOT a byte-golden: table names and
# ids vary per run, so it asserts on STRUCTURE (the steps succeed and the run
# completes), not an exact transcript.
#
# It needs AWS credentials and SPENDS (a small on-demand table, created and
# dropped). It is gated behind the CMake option DDBSH_TEST_LIVE_SAMPLE (OFF by
# default) so the ordinary unit build and any credential-less path never build
# or run it.
#
# Args (passed by the CTest registration):
#   $1 = cmake executable
#   $2 = the build dir to install from (CMAKE_BINARY_DIR)
#   $3 = the examples/live_demo source dir
#   $4 = the C++ compiler to build the sample with
#   $5 = CMAKE_PREFIX_PATH from the main build (locates the AWS SDK)

set -euo pipefail

CMAKE="$1"
BUILD_DIR="$2"
LIVE_SRC="$3"
SAMPLE_CXX="$4"
SDK_PREFIX_PATH="${5:-}"

REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-us-east-1}}"

# --- Target echo + verification (testing steering requirement) --------------
ENDPOINT="dynamodb.${REGION}.amazonaws.com (real DynamoDB, no override)"
ACCESS_KEY_ID="${AWS_ACCESS_KEY_ID:-$(aws configure get aws_access_key_id 2>/dev/null || true)}"
[ -z "$ACCESS_KEY_ID" ] && ACCESS_KEY_ID="(unresolved)"
echo "=== live_demo_sample target ==="
echo "  region:        $REGION"
echo "  endpoint:      $ENDPOINT"
echo "  access key id: $ACCESS_KEY_ID"
if [ "$ACCESS_KEY_ID" = "(unresolved)" ]; then
    echo "error: could not resolve an AWS access key id; refusing to run against an unknown target." >&2
    exit 1
fi

PREFIX="$(mktemp -d)"
WORK="$(mktemp -d)"
cleanup() { rm -rf "$PREFIX" "$WORK"; }
trap cleanup EXIT

echo "=== installing libddbsh + ddbsh::aws to $PREFIX ==="
"$CMAKE" --install "$BUILD_DIR" --prefix "$PREFIX"

echo "=== configuring + building examples/live_demo via find_package(ddbsh) ==="
"$CMAKE" -S "$LIVE_SRC" -B "$WORK" \
    -DCMAKE_PREFIX_PATH="${PREFIX};${SDK_PREFIX_PATH}" \
    -DCMAKE_CXX_COMPILER="$SAMPLE_CXX"
"$CMAKE" --build "$WORK"

echo "=== running live_demo against real DynamoDB ($REGION) ==="
set +e
"$WORK/live_demo" > "$WORK/out.txt" 2>&1
RC=$?
set -e
cat "$WORK/out.txt"

if [ "$RC" -ne 0 ]; then
    echo "live_demo sample: FAIL (exit $RC)" >&2
    exit 1
fi
if ! grep -q "live demo complete" "$WORK/out.txt"; then
    echo "live_demo sample: FAIL (did not reach completion marker)" >&2
    exit 1
fi
echo "live_demo sample: PASS"
