#!/usr/bin/env node
// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// ddbsh-node.mjs — a CLI-shaped Node front end over the browser edition
// (P9 §4.2, D-P9-02-4). It exists so the golden suite can drive the browser
// code path exactly the way it drives the native `ddbsh` binary
// (tests/golden/golden-runner.sh, DDBSH_SHELL_BIN).
//
// It loads the SAME files the page loads — ddbsh.js/ddbsh.wasm, the vendored
// SDK bundle, sdk-bridge.mjs, app-core.mjs — and pushes credentials through the
// same parseCredentials + setCredentials validation the page uses, so an
// `AKIA…` key fails here exactly as it fails in the page. Only where the
// credential text comes from differs (a file instead of a textarea).
//
// CLI surface (the subset the golden runner uses):
//   -c <cmds>   run a command string        -f <file>  run a file
//   -e          abort on the first error    (neither: read all of stdin)
//   -q, -d, -s  accepted; this shim never prints a banner, -d is a no-op in
//               the native CLI too, and the runner does the -s sort itself.
// Output goes to stdout/stderr in the engine's emission order; the exit code is
// the CLI's (last executed statement).
//
// Environment:
//   DDBSH_WEB_CREDS_FILE   JSON: { "<profile>": {AccessKeyId, SecretAccessKey,
//                          SessionToken, Expiration}, ... } — short-term STS
//                          credentials minted by devtools/run-web-golden.
//   AWS_PROFILE -> AWS_DEFAULT_PROFILE -> "default"   which entry to use
//   AWS_REGION -> AWS_DEFAULT_REGION -> "us-east-1"   the initial region
// Needs Node >= 20 (the SDK's floor; emsdk's bundled Node 24 is used by the
// devtools scripts).

import { readFileSync, writeSync } from "node:fs";
import { getSystemErrorMessage } from "node:util";

import * as sdk from "./vendor/aws-sdk/aws-sdk-dynamodb.mjs";
import { ALLOWED_HOSTS, REGIONS, SDK_VERSION } from "./vendor/aws-sdk/allowed-hosts.mjs";
import { parseCredentials } from "./app-core.mjs";
import { createSdkBridge, installHooks } from "./sdk-bridge.mjs";
import createDdbsh from "./ddbsh.js";

const USAGE = 'Usage: ddbsh-node.mjs [-q] [-e] [-d] [-s] [-c "<cmds>" | -f <file>]\n';

// Write all of `text` to `fd` (writeSync may write a pipe partially).
function writeAll(fd, text) {
    const buf = Buffer.from(text, "utf8");
    let off = 0;
    while (off < buf.length) {
        try {
            off += writeSync(fd, buf, off, buf.length - off);
        } catch (e) {
            if (e.code !== "EAGAIN") {
                throw e;
            }
        }
    }
}

function die(message, code = 1) {
    writeAll(2, message);
    process.exit(code);
}

// ---- arguments (getopt-style: clustered flags, attached or separate args) ---
let cmdstring = null;
let inputfile = null;
let abortOnError = false;
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (!a.startsWith("-") || a === "-") {
        die(USAGE);
    }
    for (let j = 1; j < a.length; j += 1) {
        const ch = a[j];
        if (ch === "q" || ch === "d" || ch === "s") {
            continue;
        }
        if (ch === "e") {
            abortOnError = true;
            continue;
        }
        if (ch === "c" || ch === "f") {
            let value = a.slice(j + 1);
            if (value === "") {
                i += 1;
                if (i >= argv.length) {
                    die(USAGE);
                }
                value = argv[i];
            }
            if (ch === "c") {
                cmdstring = value;
            } else {
                inputfile = value;
            }
            break;
        }
        die(USAGE);
    }
}
if (cmdstring !== null && inputfile !== null) {
    // The native CLI prints this to stdout and exits 1.
    writeAll(1, "ddbsh: Cannot specify both -c and -f.\n");
    process.exit(1);
}
if (process.env.DDBSH_ENDPOINT_OVERRIDE) {
    die("ddbsh-node: DDBSH_ENDPOINT_OVERRIDE is not supported by the browser edition; use CONNECT\n");
}

// ---- input --------------------------------------------------------------
let text;
if (cmdstring !== null) {
    text = cmdstring;
} else if (inputfile !== null) {
    try {
        text = readFileSync(inputfile, "utf8");
    } catch (e) {
        // perror(<path>) parity: "<path>: <strerror>".
        const msg = typeof e.errno === "number" ? getSystemErrorMessage(e.errno) : String(e.message);
        die(`${inputfile}: ${msg.charAt(0).toUpperCase()}${msg.slice(1)}\n`);
    }
} else {
    text = readFileSync(0, "utf8");
}

// ---- credentials (same validation as the page) ----------------------------
const profile = process.env.AWS_PROFILE || process.env.AWS_DEFAULT_PROFILE || "default";
const region = process.env.AWS_REGION || process.env.AWS_DEFAULT_REGION || "us-east-1";
const credsFile = process.env.DDBSH_WEB_CREDS_FILE;
if (!credsFile) {
    die("ddbsh-node: DDBSH_WEB_CREDS_FILE is not set (devtools/run-web-golden mints it)\n");
}
let entry;
try {
    entry = JSON.parse(readFileSync(credsFile, "utf8"))[profile];
} catch (e) {
    die(`ddbsh-node: cannot read ${credsFile}: ${e.message}\n`);
}
if (!entry) {
    die(`ddbsh-node: no credentials for profile ${profile} in DDBSH_WEB_CREDS_FILE\n`);
}
const parsed = parseCredentials(JSON.stringify({ Credentials: entry }));
if (!parsed.ok) {
    die(`ddbsh-node: profile ${profile}: ${parsed.message}\n`);
}

const bridge = createSdkBridge({ sdk, allowedHosts: ALLOWED_HOSTS, regions: REGIONS,
    sdkVersion: SDK_VERSION, region });
const set = bridge.setCredentials(parsed.credentials);
if (!set.ok) {
    die(`ddbsh-node: profile ${profile}: ${set.message}\n`);
}

// ---- run --------------------------------------------------------------------
const Module = await createDdbsh();
installHooks(Module, bridge);
const session = new Module.DdbshSession(region);
let result = session.runScript(text, abortOnError);
if (result && typeof result.then === "function") {
    result = await result; // Asyncify surfaces a suspending call as a Promise.
}
for (const seg of result.segments) {
    writeAll(seg.stream === "stderr" ? 2 : 1, seg.text);
}
process.exit(result.status);
