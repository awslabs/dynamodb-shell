# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# devtools/lib-web.sh — shared setup for the browser-edition devtools
# (run-web-smoke, run-web-golden, serve-web). Source it; do not execute it.
#
# Provides:
#   web_setup_emsdk      locate the pinned emsdk, pick a python >= 3.10 for its
#                        scripts, put emcc/emcmake and emsdk's Node on PATH, and
#                        set NODE_BIN (Node >= 20 is required by the SDK bundle).
#   web_build_bundle     configure (once) + build the wasm-release preset and
#                        set WEB_OUT to the staged static bundle dir.
#   web_offline_gates    run the offline web gates against WEB_OUT: the glue
#                        no-eval check (D-P9-02-9), app-core.test.mjs and
#                        sdk-bridge.test.mjs. Returns nonzero on any failure.
#
# Env overrides: EMSDK (emsdk root; default $HOME/emsdk), EMSDK_PYTHON,
# DDBSH_WASM_BUILD_DIR.

web_setup_emsdk() {
    EMSDK_ROOT="${EMSDK:-$HOME/emsdk}"
    if [ ! -f "$EMSDK_ROOT/emsdk_env.sh" ]; then
        echo "error: emsdk not found at '$EMSDK_ROOT' (set EMSDK or install per web/README.md)." >&2
        return 1
    fi
    if [ -z "${EMSDK_PYTHON:-}" ]; then
        if python3 -c 'import sys; sys.exit(0 if sys.version_info>=(3,10) else 1)' 2>/dev/null; then
            EMSDK_PYTHON="$(command -v python3)"
        else
            for cand in python3.13 python3.12 python3.11 python3.10; do
                if command -v "$cand" >/dev/null 2>&1; then
                    EMSDK_PYTHON="$(command -v "$cand")"
                    break
                fi
            done
        fi
    fi
    if [ -z "${EMSDK_PYTHON:-}" ]; then
        echo "error: no python >= 3.10 found for the emsdk scripts (set EMSDK_PYTHON)." >&2
        return 1
    fi
    export EMSDK_PYTHON
    export EMSDK="$EMSDK_ROOT"
    local node_bin
    node_bin="$(dirname "$(ls -d "$EMSDK_ROOT"/node/*/bin/node 2>/dev/null | sort -V | tail -1)" 2>/dev/null || true)"
    export PATH="$EMSDK_ROOT:$EMSDK_ROOT/upstream/emscripten${node_bin:+:$node_bin}:$PATH"
    if ! command -v emcmake >/dev/null 2>&1; then
        echo "error: emcmake not on PATH after emsdk setup (EMSDK=$EMSDK_ROOT)." >&2
        return 1
    fi
    NODE_BIN="$(command -v node)"
    if ! "$NODE_BIN" -e 'process.exit(Number(process.versions.node.split(".")[0]) >= 20 ? 0 : 1)'; then
        echo "error: Node >= 20 required by the AWS SDK bundle (found $("$NODE_BIN" --version) at $NODE_BIN)." >&2
        return 1
    fi
}

web_build_bundle() {
    local repo="$1"
    WASM_BUILD="${DDBSH_WASM_BUILD_DIR:-$repo/build-preset/wasm-release}"
    if [ ! -f "$WASM_BUILD/CMakeCache.txt" ]; then
        (cd "$repo" && emcmake cmake --preset wasm-release) || return 1
    fi
    cmake --build "$WASM_BUILD" || return 1
    WEB_OUT="$WASM_BUILD/web"
    local f
    for f in ddbsh.js ddbsh.wasm index.html app.js sdk-bridge.mjs ddbsh-node.mjs \
             vendor/aws-sdk/aws-sdk-dynamodb.mjs vendor/aws-sdk/allowed-hosts.mjs; do
        if [ ! -f "$WEB_OUT/$f" ]; then
            echo "error: web bundle incomplete: $WEB_OUT/$f missing" >&2
            return 1
        fi
    done
}

web_offline_gates() {
    local rc=0
    echo "--- glue no-eval check (D-P9-02-9) ---"
    if grep -nE 'newFunc\(Function|new Function|[^A-Za-z_.]eval\(' "$WEB_OUT/ddbsh.js"; then
        echo "FAIL: ddbsh.js contains dynamic code; the page CSP (script-src 'self' 'wasm-unsafe-eval') would block it" >&2
        rc=1
    else
        echo "ok: ddbsh.js has no eval / new Function"
    fi
    echo "--- app-core.test.mjs ---"
    (cd "$WEB_OUT" && "$NODE_BIN" app-core.test.mjs) || rc=1
    echo "--- sdk-bridge.test.mjs ---"
    (cd "$WEB_OUT" && "$NODE_BIN" sdk-bridge.test.mjs) || rc=1
    return $rc
}

# web_mint_credentials <dir> <profile>...
# Mint short-term STS session credentials for each profile (aws sts
# get-session-token, DDBSH_WEB_CRED_SECONDS, default 7200) and write
# <dir>/creds.json = { "<profile>": {AccessKeyId, SecretAccessKey,
# SessionToken, Expiration}, ... } — the DDBSH_WEB_CREDS_FILE format
# web/ddbsh-node.mjs reads. <dir> must be private (0700); files are 0600.
# Secrets never appear on a command line or in a log. A minting failure is an
# infrastructure failure (nonzero return), never a skip.
web_mint_credentials() {
    local dir="$1"
    shift
    local seconds="${DDBSH_WEB_CRED_SECONDS:-7200}"
    local p
    (
        umask 077
        for p in "$@"; do
            if ! AWS_STS_REGIONAL_ENDPOINTS=regional aws sts get-session-token \
                    --duration-seconds "$seconds" --profile "$p" --region us-east-1 \
                    --output json >"$dir/sts-$p.json"; then
                echo "error: could not mint short-term credentials for profile '$p' (aws sts get-session-token)." >&2
                exit 1
            fi
        done
        "$NODE_BIN" -e '
            const fs = require("node:fs");
            const [dir, ...profiles] = process.argv.slice(1);
            const out = {};
            for (const p of profiles) {
                const c = JSON.parse(fs.readFileSync(`${dir}/sts-${p}.json`, "utf8")).Credentials;
                if (!c || !/^ASIA/.test(c.AccessKeyId || "") || !c.SessionToken) {
                    console.error(`error: profile ${p}: STS did not return short-term (ASIA…) credentials`);
                    process.exit(1);
                }
                out[p] = { AccessKeyId: c.AccessKeyId, SecretAccessKey: c.SecretAccessKey,
                           SessionToken: c.SessionToken, Expiration: c.Expiration };
            }
            fs.writeFileSync(`${dir}/creds.json`, JSON.stringify(out), { mode: 0o600 });
        ' "$dir" "$@"
        rc=$?
        rm -f "$dir"/sts-*.json
        exit $rc
    )
}

# web_creds_field <creds.json> <profile> <field>
# Print one non-secret field (AccessKeyId or Expiration) for the target echo.
web_creds_field() {
    "$NODE_BIN" -e '
        const [f, p, k] = process.argv.slice(1);
        if (k !== "AccessKeyId" && k !== "Expiration") { process.exit(2); }
        process.stdout.write(String(JSON.parse(require("node:fs").readFileSync(f, "utf8"))[p][k]));
    ' "$1" "$2" "$3"
}

# web_caller_arn <creds.json> <profile>
# `aws sts get-caller-identity` run WITH the minted credentials (passed through
# the environment of that one command, never on its command line).
web_caller_arn() {
    "$NODE_BIN" -e '
        const { execFileSync } = require("node:child_process");
        const [f, p] = process.argv.slice(1);
        const c = JSON.parse(require("node:fs").readFileSync(f, "utf8"))[p];
        const env = { ...process.env, AWS_ACCESS_KEY_ID: c.AccessKeyId,
            AWS_SECRET_ACCESS_KEY: c.SecretAccessKey, AWS_SESSION_TOKEN: c.SessionToken };
        delete env.AWS_PROFILE; delete env.AWS_DEFAULT_PROFILE;
        try {
            process.stdout.write(execFileSync("aws", ["sts", "get-caller-identity", "--query", "Arn",
                "--output", "text", "--region", "us-east-1"], { env, encoding: "utf8" }).trim());
        } catch { process.stdout.write("(sts unavailable)"); }
    ' "$1" "$2"
}

# web_golden_lock <repo>
# Hold an exclusive lock for the rest of the calling script so a native golden
# run (devtools/run-tests) and a web golden run (devtools/run-web-golden) never
# overlap: both create and drop the same ddbsh_test_* tables in one account.
# Uses flock(1) when available (Linux); elsewhere prints a warning.
web_golden_lock() {
    local lock="$1/build-golden.lock"
    if command -v flock >/dev/null 2>&1; then
        exec 9>"$lock"
        if ! flock -n 9; then
            echo "error: another golden run holds $lock; the native and web golden suites share the ddbsh_test_* tables and must not run concurrently." >&2
            return 1
        fi
    else
        echo "warning: flock(1) not found; not guarding against a concurrent golden run." >&2
    fi
}
