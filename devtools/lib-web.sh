# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# devtools/lib-web.sh — shared setup for the browser-edition devtools
# (run-web-smoke, run-web-golden, serve-web, vendor-aws-sdk-js). Source it; do
# not execute it.
#
# Provides:
#   web_select_node      make `node` on PATH a Node >= 20 (the floor of the
#                        vendored AWS SDK for JavaScript): keep a Node 20+ that
#                        is already on PATH, else put emsdk's newest bundled
#                        Node first. vendor-aws-sdk-js and web_setup_emsdk both
#                        use it, so they always pick the same Node.
#   web_setup_emsdk      locate the pinned emsdk, pick a python >= 3.10 for its
#                        scripts, put emcc/emcmake on PATH, pick Node with
#                        web_select_node, and set NODE_BIN.
#   web_build_bundle     configure (once) + build the wasm-release preset and
#                        its packaging step (ddbsh-web-package), and set
#                        WEB_OUT to the staged static bundle dir.
#   web_offline_gates    run the offline web gates: against WEB_OUT, the glue
#                        no-eval check (D-P9-02-9), app-core.test.mjs,
#                        sdk-bridge.test.mjs and package.test.mjs; against the
#                        sources, the browser-storage gate (D-P9-4) and the
#                        upload-command drift gate (deploy.sh vs
#                        docs/HOSTING.md). Needs WEB_OUT and NODE_BIN. Every
#                        caller runs the same set: run-web-smoke,
#                        run-web-golden and CI (.github/workflows/
#                        web-bundle.yml). Returns nonzero on any failure.
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
    export PATH="$EMSDK_ROOT:$EMSDK_ROOT/upstream/emscripten:$PATH"
    if ! command -v emcmake >/dev/null 2>&1; then
        echo "error: emcmake not on PATH after emsdk setup (EMSDK=$EMSDK_ROOT)." >&2
        return 1
    fi
    web_select_node "$EMSDK_ROOT" || return 1
    NODE_BIN="$(command -v node)"
}

# web_node_ok <node>
# True when <node> runs and is Node 20 or newer, the floor of the vendored AWS
# SDK for JavaScript (SDK >= 3.950). False for an empty argument.
web_node_ok() {
    [ -n "$1" ] &&
        "$1" -e 'process.exit(Number(process.versions.node.split(".")[0]) >= 20 ? 0 : 1)' \
            >/dev/null 2>&1
}

# web_node_version <node>
# "<version> (<path>)" for an error message, or "no Node" for an empty
# argument or a node that does not run.
web_node_version() {
    local v
    if [ -n "$1" ] && v="$("$1" --version 2>/dev/null)"; then
        printf '%s (%s)' "$v" "$1"
    else
        printf 'no Node'
    fi
}

# web_select_node <emsdk-root>
# Make `node` on PATH a Node 20 or newer. A Node 20+ already on PATH is kept:
# CI's actions/setup-node, or a developer's own install. Only when PATH has no
# Node 20+ does emsdk's newest bundled Node go first on PATH. The Node that
# emsdk bundles depends on the emsdk checkout, not on the Emscripten version:
# for 3.1.61, CI's setup-emsdk v16 brings Node 18.20.3 and a current emsdk
# checkout brings Node 24. So emsdk's Node never shadows a newer one (P11 F1).
# Returns 1, naming both candidates, when neither is Node 20+.
web_select_node() {
    local emsdk_root="$1" path_node emsdk_node emsdk_bin
    path_node="$(command -v node || true)"
    if web_node_ok "$path_node"; then
        return 0
    fi
    emsdk_node="$(ls -d "$emsdk_root"/node/*/bin/node 2>/dev/null | sort -V | tail -1 || true)"
    if web_node_ok "$emsdk_node"; then
        emsdk_bin="$(dirname "$emsdk_node")"
        PATH="$emsdk_bin:$PATH"
        export PATH
        return 0
    fi
    echo "error: Node >= 20 required; found $(web_node_version "$path_node") on PATH and $(web_node_version "$emsdk_node") in emsdk ($emsdk_root). Put Node 20+ on PATH, or install emsdk per web/README.md." >&2
    return 1
}

web_build_bundle() {
    local repo="$1"
    WASM_BUILD="${DDBSH_WASM_BUILD_DIR:-$repo/build-preset/wasm-release}"
    if [ ! -f "$WASM_BUILD/CMakeCache.txt" ]; then
        (cd "$repo" && emcmake cmake --preset wasm-release) || return 1
    fi
    cmake --build "$WASM_BUILD" || return 1
    # The packaging step (P10): dist/ddbsh-web-<version>/ + zip + SHA256SUMS,
    # checked by package.test.mjs in web_offline_gates.
    cmake --build "$WASM_BUILD" --target ddbsh-web-package || return 1
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
    local rc=0 repo storage_re storage_ok bad deploy_cmds guide_cmds upload_re
    # The source gates read the repository, found from this file's location
    # (devtools/), so callers pass nothing beyond WEB_OUT and NODE_BIN.
    repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    if [ -z "${WEB_OUT:-}" ] || [ -z "${NODE_BIN:-}" ]; then
        echo "error: web_offline_gates needs WEB_OUT and NODE_BIN" >&2
        return 1
    fi
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
    echo "--- package.test.mjs ---"
    (cd "$WEB_OUT" && "$NODE_BIN" package.test.mjs) || rc=1

    # Credentials stay in the tab's memory (D-P9-4): sdk-bridge.mjs never
    # touches browser storage, and app.js uses it only for the command
    # history (read, write, and the "Clear history" removal).
    echo "--- storage grep gate (D-P9-4) ---"
    storage_re='localStorage|sessionStorage|indexedDB|document\.cookie'
    storage_ok=1
    bad="$(grep -nE "$storage_re" "$repo/web/sdk-bridge.mjs" || true)"
    if [ -n "$bad" ]; then
        echo "$bad"
        echo "FAIL: sdk-bridge.mjs touches browser storage" >&2
        storage_ok=0
    fi
    bad="$(grep -nE "$storage_re" "$repo/web/app.js" | grep -vE 'localStorage\.(getItem|setItem|removeItem)\(HISTORY_KEY' | grep -vE '^[0-9]+:\s*//' || true)"
    if [ -n "$bad" ]; then
        echo "$bad"
        echo "FAIL: app.js uses browser storage beyond the command history" >&2
        storage_ok=0
    fi
    if [ "$storage_ok" -eq 1 ]; then
        echo "ok: no credential-capable storage use"
    else
        rc=1
    fi

    # The upload commands exist twice by design: web/hosting/aws/deploy.sh
    # runs them and docs/HOSTING.md shows them for copy-paste (P10). Each sets
    # the Content-Type browsers need, so the two copies must not drift apart.
    echo "--- upload-command drift gate (deploy.sh vs docs/HOSTING.md) ---"
    upload_re='^aws (s3 sync|cloudfront create-invalidation) '
    deploy_cmds="$(grep -E "$upload_re" "$repo/web/hosting/aws/deploy.sh" || true)"
    guide_cmds="$(grep -E "$upload_re" "$repo/docs/HOSTING.md" || true)"
    if [ "$(printf '%s\n' "$deploy_cmds" | grep -c .)" -ne 7 ] || [ "$deploy_cmds" != "$guide_cmds" ]; then
        # diff exits 1 here; `|| true` keeps a caller's errexit (CI's
        # bash -eo pipefail) from ending the function before the FAIL line.
        diff <(printf '%s\n' "$deploy_cmds") <(printf '%s\n' "$guide_cmds") || true
        echo "FAIL: deploy.sh and docs/HOSTING.md must carry the same seven upload commands" >&2
        rc=1
    else
        echo "ok: deploy.sh and docs/HOSTING.md carry the same seven upload commands"
    fi
    return $rc
}

# web_mint_credentials <dir> <profile>...
# Mint short-term STS session credentials for each profile (aws sts
# get-session-token, DDBSH_WEB_CRED_SECONDS, default 7200; for a profile that
# cannot call it — a role_arn/source_profile or SSO profile — fall back to
# `aws configure export-credentials --format process`, which must yield ASIA…
# credentials) and write
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
        # The STS JSON holds secrets: remove it however this subshell exits.
        trap 'rm -f "$dir"/sts-*.json "$dir"/sts-*.err' EXIT
        for p in "$@"; do
            if ! AWS_STS_REGIONAL_ENDPOINTS=regional aws sts get-session-token \
                    --duration-seconds "$seconds" --profile "$p" --region us-east-1 \
                    --output json >"$dir/sts-$p.json" 2>"$dir/sts-$p.err"; then
                if ! AWS_STS_REGIONAL_ENDPOINTS=regional aws configure export-credentials \
                        --profile "$p" --format process >"$dir/sts-$p.json"; then
                    cat "$dir/sts-$p.err" >&2
                    echo "error: could not mint short-term credentials for profile '$p' (sts get-session-token and configure export-credentials both failed)." >&2
                    exit 1
                fi
            fi
            rm -f "$dir/sts-$p.err"
        done
        "$NODE_BIN" -e '
            const fs = require("node:fs");
            const [dir, ...profiles] = process.argv.slice(1);
            const out = {};
            for (const p of profiles) {
                const doc = JSON.parse(fs.readFileSync(`${dir}/sts-${p}.json`, "utf8"));
                const c = doc.Credentials || doc; // STS JSON, or credential-process JSON
                if (!c || !/^ASIA/.test(c.AccessKeyId || "") || !c.SessionToken) {
                    console.error(`error: profile ${p}: STS did not return short-term (ASIA…) credentials`);
                    process.exit(1);
                }
                out[p] = { AccessKeyId: c.AccessKeyId, SecretAccessKey: c.SecretAccessKey,
                           SessionToken: c.SessionToken, Expiration: c.Expiration };
            }
            fs.writeFileSync(`${dir}/creds.json`, JSON.stringify(out), { mode: 0o600 });
        ' "$dir" "$@"
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
