// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// package.test.mjs — offline checks of the browser edition's packaging step
// (P10, D-P10-7, G4).
//
// Run from the staged build output after building the ddbsh-web-package
// target:
//
//   cd build-preset/wasm-release/web && node package.test.mjs [../dist]
//
// It checks three things:
//   1. bundle-files.txt (the publish allowlist) matches what the page actually
//      loads: index.html's src/href references, the ES-module import graph
//      from app.js (static and dynamic imports), and the module glue's
//      `new URL("…", import.meta.url)` fetch of ddbsh.wasm. The only listed
//      files the page does not load are the two third-party notices.
//   2. the packaged output in the dist dir: the ddbsh-web-<version>/ folder
//      holds exactly the allowlisted files, byte-identical to the staged build;
//      SHA256SUMS covers every one of them and the zip; and the zip holds
//      exactly the folder's files with the same contents.
//   3. package.cmake (staged beside this file) refuses each kind of bad input
//      with its error: a missing -D variable, a path that is not plain and
//      relative, a duplicate, an empty allowlist, a file the build lacks. It
//      runs in a scratch directory under the OS temp dir and needs cmake on
//      PATH (or CMAKE_COMMAND).
//
// Zero dependencies (plain asserts + an exit code, like the other web tests).

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { lstatSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, posix, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { inflateRawSync } from "node:zlib";

const HERE = dirname(fileURLToPath(import.meta.url));
const DIST = resolve(process.argv[2] || join(HERE, "..", "dist"));
// Published but not loaded by the page: the bundled SDK's license notices and
// provenance record.
const NOTICES = ["vendor/aws-sdk/PROVENANCE.json", "vendor/aws-sdk/THIRD-PARTY-NOTICES.txt"];

let failures = 0;
function check(cond, label) {
    if (cond) {
        console.log(`  ok   ${label}`);
    } else {
        console.error(`  FAIL ${label}`);
        failures += 1;
    }
}
function eq(actual, expected, label) {
    const a = JSON.stringify(actual);
    const e = JSON.stringify(expected);
    check(a === e, `${label} — expected ${e}, got ${a}`);
}
const sha256 = (buf) => createHash("sha256").update(buf).digest("hex");
const sorted = (xs) => [...xs].sort();

// ---- the allowlist -----------------------------------------------------------
console.log("allowlist");
const allowlist = readFileSync(join(HERE, "bundle-files.txt"), "utf8")
    .split(/\r?\n/).map((l) => l.trim()).filter((l) => l !== "" && !l.startsWith("#"));
eq(allowlist.length, 11, "bundle-files.txt lists 11 files");
eq(new Set(allowlist).size, allowlist.length, "no duplicate entries");
check(allowlist.every((p) => !p.startsWith("/") && !p.includes("\\") &&
    !p.split("/").some((s) => s === "." || s === "..")), "every entry is a plain relative path");

// ---- the page's load graph ---------------------------------------------------
// Local references only: relative URLs (no scheme, not root-relative, not a
// bare module specifier) resolved against the referencing file.
console.log("load graph");
function isLocal(ref, { bareIsLocal }) {
    if (ref === "" || ref.startsWith("/") || ref.startsWith("#") || /^[a-z][a-z0-9+.-]*:/i.test(ref)) {
        return false;
    }
    return bareIsLocal || ref.startsWith("./") || ref.startsWith("../");
}
function resolveRef(from, ref) {
    return posix.normalize(posix.join(posix.dirname(from), ref.split(/[?#]/)[0]));
}
function referencesOf(file, text) {
    const refs = [];
    if (file.endsWith(".html")) {
        for (const m of text.matchAll(/\s(?:src|href)="([^"]*)"/g)) {
            if (isLocal(m[1], { bareIsLocal: true })) {
                refs.push(resolveRef(file, m[1]));
            }
        }
    } else if (/\.m?js$/.test(file)) {
        const patterns = [
            /\bfrom\s*["']([^"']+)["']/g,          // import … from "x" / export … from "x"
            /\bimport\s*["']([^"']+)["']/g,         // import "x"
            /\bimport\s*\(\s*["']([^"']+)["']\s*\)/g, // import("x")
        ];
        for (const re of patterns) {
            for (const m of text.matchAll(re)) {
                if (isLocal(m[1], { bareIsLocal: false })) {
                    refs.push(resolveRef(file, m[1]));
                }
            }
        }
        // new URL("x", import.meta.url): a relative URL, so a bare name is local.
        for (const m of text.matchAll(/new URL\(\s*["']([^"']+)["']\s*,\s*import\.meta\.url\s*\)/g)) {
            if (!m[1].endsWith("/") && isLocal(m[1], { bareIsLocal: true })) {
                refs.push(resolveRef(file, m[1]));
            }
        }
    } else if (file.endsWith(".css")) {
        for (const m of text.matchAll(/url\(\s*["']?([^"')]+)["']?\s*\)/g)) {
            if (isLocal(m[1], { bareIsLocal: true })) {
                refs.push(resolveRef(file, m[1]));
            }
        }
    }
    return refs;
}
const loaded = new Set(["index.html"]);
const queue = ["index.html"];
while (queue.length > 0) {
    const file = queue.shift();
    let text;
    try {
        text = readFileSync(join(HERE, file), "utf8");
    } catch {
        check(false, `the page references ${file}, which is not in the build output`);
        continue;
    }
    for (const ref of referencesOf(file, text)) {
        if (!loaded.has(ref)) {
            loaded.add(ref);
            queue.push(ref);
        }
    }
}
for (const f of loaded) {
    check(allowlist.includes(f), `loaded by the page and allowlisted: ${f}`);
}
eq(sorted(allowlist.filter((f) => !loaded.has(f))), sorted(NOTICES),
    "the only allowlisted files the page does not load are the SDK notices");

// ---- the packaged output -----------------------------------------------------
console.log(`packaged output (${DIST})`);
let entries = [];
try {
    entries = readdirSync(DIST);
} catch {
    check(false, `${DIST} exists (build the ddbsh-web-package target first)`);
}
const folders = entries.filter((e) => /^ddbsh-web-\d+\.\d+\.\d+$/.test(e) && lstatSync(join(DIST, e)).isDirectory());
eq(folders.length, 1, "exactly one ddbsh-web-<version> folder");
const bundle = folders[0] || "ddbsh-web-missing";
eq(sorted(entries), sorted([bundle, `${bundle}.zip`, "SHA256SUMS"]),
    "the dist dir holds the folder, its zip and SHA256SUMS, nothing else");

function walk(root, rel = "") {
    const out = [];
    for (const name of readdirSync(join(root, rel))) {
        const p = rel === "" ? name : `${rel}/${name}`;
        const st = lstatSync(join(root, p));
        if (st.isSymbolicLink()) {
            out.push(`${p} (symlink)`);
        } else if (st.isDirectory()) {
            out.push(...walk(root, p));
        } else {
            out.push(p);
        }
    }
    return out;
}
const bundleDir = join(DIST, bundle);
const packaged = folders.length === 1 ? walk(bundleDir) : [];
eq(sorted(packaged), sorted(allowlist), "the folder holds exactly the allowlisted files");
const hashes = new Map();
for (const f of allowlist) {
    try {
        const h = sha256(readFileSync(join(bundleDir, f)));
        hashes.set(`${bundle}/${f}`, h);
        check(h === sha256(readFileSync(join(HERE, f))), `packaged ${f} is byte-identical to the build output`);
    } catch (e) {
        check(false, `packaged ${f} is readable (${e.message})`);
    }
}

console.log("SHA256SUMS");
const sums = new Map();
let sumsText = "";
try {
    sumsText = readFileSync(join(DIST, "SHA256SUMS"), "utf8");
} catch {
    check(false, "SHA256SUMS is readable");
}
for (const line of sumsText.split("\n").filter((l) => l !== "")) {
    const m = line.match(/^([0-9a-f]{64}) {2}(\S+)$/);
    check(m !== null, `sha256sum -c format: ${line.slice(0, 80)}`);
    if (m) {
        sums.set(m[2], m[1]);
    }
}
let zip = Buffer.alloc(0);
try {
    zip = readFileSync(join(DIST, `${bundle}.zip`));
} catch {
    check(false, `${bundle}.zip is readable`);
}
eq(sorted(sums.keys()), sorted([...hashes.keys(), `${bundle}.zip`]),
    "SHA256SUMS lists every packaged file and the zip");
for (const [path, h] of hashes) {
    check(sums.get(path) === h, `SHA256SUMS matches ${path}`);
}
check(sums.get(`${bundle}.zip`) === sha256(zip), `SHA256SUMS matches ${bundle}.zip`);

// ---- the zip ------------------------------------------------------------------
// A minimal reader: the end-of-central-directory record, the central directory,
// then each local header's data (stored or deflated).
console.log("zip");
function readZip(buf) {
    const eocd = buf.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
    if (eocd < 0) {
        throw new Error("no end-of-central-directory record");
    }
    const count = buf.readUInt16LE(eocd + 10);
    let p = buf.readUInt32LE(eocd + 16);
    const members = [];
    for (let i = 0; i < count; i += 1) {
        if (buf.readUInt32LE(p) !== 0x02014b50) {
            throw new Error(`bad central directory entry ${i}`);
        }
        const method = buf.readUInt16LE(p + 10);
        const csize = buf.readUInt32LE(p + 20);
        const nameLen = buf.readUInt16LE(p + 28);
        const extraLen = buf.readUInt16LE(p + 30);
        const commentLen = buf.readUInt16LE(p + 32);
        const local = buf.readUInt32LE(p + 42);
        const name = buf.toString("utf8", p + 46, p + 46 + nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (buf.readUInt32LE(local) !== 0x04034b50) {
            throw new Error(`bad local header for ${name}`);
        }
        const start = local + 30 + buf.readUInt16LE(local + 26) + buf.readUInt16LE(local + 28);
        const raw = buf.subarray(start, start + csize);
        let data;
        if (method === 0) {
            data = raw;
        } else if (method === 8) {
            data = inflateRawSync(raw);
        } else {
            throw new Error(`${name}: unsupported compression method ${method}`);
        }
        members.push({ name, data });
    }
    return members;
}
try {
    const members = readZip(zip).filter((m) => !m.name.endsWith("/")); // skip directory entries
    eq(sorted(members.map((m) => m.name)), sorted(hashes.keys()),
        "the zip holds exactly the folder's files, under the folder name");
    for (const m of members) {
        check(sha256(m.data) === hashes.get(m.name), `zip member ${m.name} matches the folder`);
    }
} catch (e) {
    check(false, `${bundle}.zip is a readable zip (${e.message})`);
}

// ---- package.cmake refuses bad input -------------------------------------------
// Each FATAL_ERROR path of the packaging script, run in CMake script mode on a
// scratch build dir and allowlist under the OS temp dir, never on the real dist
// dir (package.cmake removes DIST_DIR before it copies). A good allowlist runs
// first, so a refusal below is the script's, not a broken scratch setup.
console.log("package.cmake refuses bad input");
{
    const cmake = process.env.CMAKE_COMMAND || "cmake";
    const script = join(HERE, "package.cmake");
    const scratch = mkdtempSync(join(tmpdir(), "ddbsh-package-test-"));
    try {
        const webOut = join(scratch, "web");
        const distDir = join(scratch, "dist");
        mkdirSync(join(webOut, "sub"), { recursive: true });
        writeFileSync(join(webOut, "a.txt"), "a\n");
        writeFileSync(join(webOut, "sub", "b.txt"), "b\n");
        const runPackage = (manifestText, omit = "") => {
            const manifest = join(scratch, "bundle-files.txt");
            writeFileSync(manifest, manifestText);
            // CMake takes forward slashes on every platform.
            const slash = (p) => p.split("\\").join("/");
            const defs = { WEB_OUT: slash(webOut), MANIFEST: slash(manifest), DIST_DIR: slash(distDir),
                BUNDLE_NAME: "ddbsh-web-0.0.0" };
            const args = Object.entries(defs).filter(([k]) => k !== omit).map(([k, v]) => `-D${k}=${v}`);
            const r = spawnSync(cmake, [...args, "-P", slash(script)], { encoding: "utf8" });
            // CMake reflows FATAL_ERROR text (wraps lines, two spaces after a
            // period), so compare with runs of whitespace collapsed.
            const output = `${r.stdout || ""}${r.stderr || ""}${r.error ? r.error.message : ""}`
                .replace(/\s+/g, " ");
            return { status: r.status, output, error: (output.match(/package\.cmake: .*$/) || [output.trim()])[0].slice(0, 160) };
        };

        const good = runPackage("# comment\na.txt\n\nsub/b.txt\n");
        check(good.status === 0, `a good allowlist is packaged (exit ${good.status}${good.status === 0 ? "" : `: ${good.output.trim()}`})`);
        let written = [];
        try {
            written = readdirSync(distDir);
        } catch {
            // checked below
        }
        eq(sorted(written), sorted(["SHA256SUMS", "ddbsh-web-0.0.0", "ddbsh-web-0.0.0.zip"]),
            "the good run writes the folder, its zip and SHA256SUMS");

        const refusals = [
            ["a missing -D variable", "a.txt\n", "BUNDLE_NAME", /-DBUNDLE_NAME=\.\.\. is required/],
            ["an absolute path", "/etc/hosts\n", "", /not a plain relative path: '\/etc\/hosts'/],
            ["a path through ..", "sub/../../outside.txt\n", "", /not a plain relative path: 'sub\/\.\.\/\.\.\/outside\.txt'/],
            ["a path through .", "./a.txt\n", "", /not a plain relative path: '\.\/a\.txt'/],
            ["a backslash", "sub\\b.txt\n", "", /not a plain relative path: 'sub\\b\.txt'/],
            ["a space", "a b.txt\n", "", /not a plain relative path: 'a b\.txt'/],
            ["a duplicate entry", "a.txt\nsub/b.txt\na.txt\n", "", /duplicate entry 'a\.txt'/],
            ["an allowlist with no files", "# only a comment\n\n", "", /lists no files/],
            ["a file the build lacks", "a.txt\nmissing.txt\n", "", /missing\.txt is missing/],
            ["a directory", "sub\n", "", /sub is missing/],
        ];
        for (const [what, text, omit, expected] of refusals) {
            const r = runPackage(text, omit);
            check(r.status !== 0 && r.status !== null && expected.test(r.output),
                `refuses ${what} (exit ${r.status}: ${r.error})`);
        }
    } finally {
        rmSync(scratch, { recursive: true, force: true });
    }
}

console.log(failures === 0
    ? "\npackage tests: PASS"
    : `\npackage tests: FAIL (${failures} assertion failure(s))`);
process.exit(failures === 0 ? 0 : 1);
