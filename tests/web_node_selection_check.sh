#!/usr/bin/env bash
# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# web_node_selection_check.sh — check which Node the browser-edition devtools
# pick (P11 F1).
#
# devtools/vendor-aws-sdk-js and devtools/lib-web.sh's web_setup_emsdk both
# need Node 20 or newer, and both pick it with web_select_node: a Node 20+
# already on PATH wins, and emsdk's newest bundled Node is only the fallback.
# The vendor script used to put emsdk's Node first unconditionally. In CI,
# setup-emsdk v16 installs emsdk 3.1.61 with Node 18.20.3, so the script ran
# Node 18 instead of setup-node's Node 22 and refused it.
#
# Stubs stand in for every Node, for npm and for emcmake, and PATH holds only
# the stubs and the few tools the scripts run, so the check needs no Node, no
# emsdk and no network. The stub npm records which node is first on PATH and
# exits 97, which stops vendor-aws-sdk-js before its npm ci.
#
# Usage: web_node_selection_check.sh <repo-root>
# Prints one line per case and exits nonzero if any case fails.

set -euo pipefail

ROOT="${1:?usage: web_node_selection_check.sh <repo-root>}"
VENDOR="$ROOT/devtools/vendor-aws-sdk-js"
LIB="$ROOT/devtools/lib-web.sh"
for f in "$VENDOR" "$LIB"; do
    if [ ! -f "$f" ]; then
        echo "web_node_selection_check: not found: $f" >&2
        exit 2
    fi
done

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
BASH_BIN="$(command -v bash)"

# The only tools on PATH besides the stubs: what the two scripts run before
# vendor-aws-sdk-js reaches npm ci.
mkdir -p "$T/tools" "$T/home"
for tool in bash cp dirname ls mktemp rm sort tail; do
    ln -s "$(command -v "$tool")" "$T/tools/$tool"
done

# make_node <dir> <version>: a stand-in for Node <version> at <dir>/node. It
# answers `--version`, and answers the scripts' `-e` version probe by its major
# version.
make_node() {
    mkdir -p "$1"
    cat >"$1/node" <<EOF
#!/usr/bin/env bash
case "\${1:-}" in
    --version) echo "v$2" ;;
    -e) [ "${2%%.*}" -ge 20 ] ;;
    *) exit 2 ;;
esac
EOF
    chmod +x "$1/node"
}

# make_emsdk <dir> <node-version>: a stand-in emsdk with one bundled Node.
make_emsdk() {
    make_node "$1/node/${2}_64bit/bin" "$2"
    mkdir -p "$1/upstream/emscripten"
    : >"$1/emsdk_env.sh"
    printf '#!/usr/bin/env bash\nexit 0\n' >"$1/upstream/emscripten/emcmake"
    chmod +x "$1/upstream/emscripten/emcmake"
}

make_node "$T/path22" 22.23.5 # CI: actions/setup-node
make_node "$T/path18" 18.20.8 # too old
make_emsdk "$T/emsdk18" 18.20.3 # CI: setup-emsdk v16, emsdk 3.1.61
make_emsdk "$T/emsdk24" 24.19.0 # a current emsdk checkout

mkdir -p "$T/npm"
cat >"$T/npm/npm" <<'EOF'
#!/usr/bin/env bash
echo "node=$(command -v node) $(node --version)" >"$NPM_LOG"
exit 97
EOF
chmod +x "$T/npm/npm"

pass=0
fail=0
result() { # <ok 0|1> <case> <detail>
    if [ "$1" -eq 0 ]; then
        echo "ok: $2"
        pass=$((pass + 1))
    else
        echo "FAIL: $2: $3" >&2
        fail=$((fail + 1))
    fi
}

# vendor_case <case> <path-prefix> <emsdk> <want-node>
# Run `vendor-aws-sdk-js --check` and expect it to reach npm ci (the stub's 97)
# with <want-node> first on PATH.
vendor_case() {
    local name="$1" prefix="$2" emsdk="$3" want="$4" rc=0 got
    rm -f "$T/npm.log"
    /usr/bin/env -i HOME="$T/home" PATH="$prefix$T/npm:$T/tools" EMSDK="$emsdk" \
        NPM_LOG="$T/npm.log" "$BASH_BIN" "$VENDOR" --check >"$T/out" 2>&1 || rc=$?
    got="$(cat "$T/npm.log" 2>/dev/null || echo "npm ci not reached")"
    if [ "$rc" -eq 97 ] && [ "$got" = "node=$want" ]; then
        result 0 "vendor-aws-sdk-js: $name"
    else
        result 1 "vendor-aws-sdk-js: $name" "exit $rc, $got, want node=$want; output: $(tr '\n' ' ' <"$T/out")"
    fi
}

# emsdk_case <case> <path-prefix> <emsdk> <want-node>
# Run web_setup_emsdk and expect NODE_BIN and `node --version` to be <want-node>.
emsdk_case() {
    local name="$1" prefix="$2" emsdk="$3" want="$4" rc=0 got
    # shellcheck disable=SC2016  # $1 and $NODE_BIN expand in the inner bash
    /usr/bin/env -i HOME="$T/home" PATH="$prefix$T/tools" EMSDK="$emsdk" EMSDK_PYTHON=unused \
        "$BASH_BIN" -c '. "$1"; web_setup_emsdk || exit 1; echo "node=$NODE_BIN $(node --version)"' \
        _ "$LIB" >"$T/out" 2>&1 || rc=$?
    got="$(tr '\n' ' ' <"$T/out")"
    if [ "$rc" -eq 0 ] && [ "$got" = "node=$want " ]; then
        result 0 "web_setup_emsdk: $name"
    else
        result 1 "web_setup_emsdk: $name" "exit $rc, output: $got, want node=$want"
    fi
}

# refuse_case <script> <path-prefix> <emsdk>
# <script> is vendor-aws-sdk-js or web_setup_emsdk. Neither candidate is Node
# 20+: expect exit 1 and an error naming both.
refuse_case() {
    local script="$1" prefix="$2" emsdk="$3" rc=0 out
    if [ "$script" = vendor-aws-sdk-js ]; then
        /usr/bin/env -i HOME="$T/home" PATH="$prefix$T/npm:$T/tools" EMSDK="$emsdk" \
            NPM_LOG="$T/npm.log" "$BASH_BIN" "$VENDOR" --check >"$T/out" 2>&1 || rc=$?
    else
        # shellcheck disable=SC2016  # $1 expands in the inner bash
        /usr/bin/env -i HOME="$T/home" PATH="$prefix$T/tools" EMSDK="$emsdk" EMSDK_PYTHON=unused \
            "$BASH_BIN" -c '. "$1"; web_setup_emsdk' _ "$LIB" >"$T/out" 2>&1 || rc=$?
    fi
    out="$(tr '\n' ' ' <"$T/out")"
    case "$out" in
        *"Node >= 20 required"*"v18.20.8 ($T/path18/node) on PATH"*"v18.20.3 ($emsdk/node/18.20.3_64bit/bin/node) in emsdk"*)
            [ "$rc" -eq 1 ] && result 0 "$script: refuses Node 18 on PATH and in emsdk" && return
            ;;
    esac
    result 1 "$script: refuses Node 18 on PATH and in emsdk" "exit $rc, output: $out"
}

# 1. CI: Node 22 on PATH, emsdk with Node 18. The PATH Node wins.
vendor_case "keeps Node 22 on PATH over emsdk's Node 18" "$T/path22:" "$T/emsdk18" \
    "$T/path22/node v22.23.5"
emsdk_case "keeps Node 22 on PATH over emsdk's Node 18" "$T/path22:" "$T/emsdk18" \
    "$T/path22/node v22.23.5"
# 2. Node 18 on PATH (an old system Node), emsdk with Node 24. emsdk's Node is used.
vendor_case "uses emsdk's Node 24 when PATH has Node 18" "$T/path18:" "$T/emsdk24" \
    "$T/emsdk24/node/24.19.0_64bit/bin/node v24.19.0"
emsdk_case "uses emsdk's Node 24 when PATH has Node 18" "$T/path18:" "$T/emsdk24" \
    "$T/emsdk24/node/24.19.0_64bit/bin/node v24.19.0"
# 3. No Node on PATH at all, emsdk with Node 24.
vendor_case "uses emsdk's Node 24 when PATH has no Node" "" "$T/emsdk24" \
    "$T/emsdk24/node/24.19.0_64bit/bin/node v24.19.0"
emsdk_case "uses emsdk's Node 24 when PATH has no Node" "" "$T/emsdk24" \
    "$T/emsdk24/node/24.19.0_64bit/bin/node v24.19.0"
# 4. Node 18 on PATH and in emsdk: both scripts refuse, naming both.
refuse_case vendor-aws-sdk-js "$T/path18:" "$T/emsdk18"
refuse_case web_setup_emsdk "$T/path18:" "$T/emsdk18"

echo "web node selection check: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
