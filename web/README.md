<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# ddbsh browser edition (Emscripten/WASM + AWS SDK for JavaScript) — developer notes

The browser edition is a static page. The ddbsh engine (the same lexer, parser,
planner, executor, renderer and `ddbsh-wire` codec the CLI links) is compiled to
WebAssembly. A vendored, pinned AWS SDK for JavaScript v3 bundle signs each
request and sends it from the tab straight to Amazon DynamoDB. There is no
server-side component.

User documentation (credentials, serving, what is refused):
[`docs/configurations/03-browser-aws.md`](../docs/configurations/03-browser-aws.md).
Publishing the page for others (packaging, the CloudFormation template,
checks, other hosts): [`docs/HOSTING.md`](../docs/HOSTING.md).
Design of record: P9-02 (`discussions/2026-11-20-P9-02-browser-js-sdk-design.md`
in the orchestration repo).

## How a statement runs

```
editor text ─► DdbshSession.run (bindings.cpp, embind)
                 └─ cli::run_batch ─► Session ─► WireClient (ddbsh-wire)
                                                   │ serialize request body
                                                   ▼
                    JsSdkTransport::send ─ Asyncify ─► Module.ddbshSend(op, body)
                                                   │ (sdk-bridge.mjs)
                                                   ▼
          DynamoDBClient.send(new <Op>Command({})) with two middlewares:
            after serializer:   body := engine body; x-amz-target := op;
                                refuse a host outside ALLOWED_HOSTS
            after deserializer: capture the raw response body
          (the SDK does credentials, endpoint, SigV4, retries, fetch)
                                                   │ raw body (success or __type)
                                                   ▼
                    WireClient parse ─► ExecOutcome ─► Renderer ─► RunResult.segments
```

The SDK never deserializes anything the engine sees, so binary values, epoch
timestamps and members the SDK does not model reach the engine's parser
unchanged, and service errors arrive as their verbatim `__type` document.

## What's here

| File | Role |
|---|---|
| `CMakeLists.txt` | the `ddbsh-wasm` target (OUTPUT_NAME `ddbsh`): `bindings.cpp` + `js_sdk_transport.cpp`, linking the engine, `ddbsh-wire` and `ddbsh-cli-core`; stages the static bundle |
| `bindings.cpp` | embind `DdbshSession`: `run(text)` (UI path, `run_batch`), `runScript(text, abortOnError)` (CLI `-c`/`-f` path, `run_source`), `region()`, `endpointOverridden()`; `RunResult {stdout, stderr, segments, ok, status}`; `WebWireClient` (retry cap 1, CONNECT-rejection notice) |
| `js_sdk_transport.{hpp,cpp}` | `JsSdkTransport`: `send` → `Module.ddbshSend` (Asyncify), `on_connect` → `Module.ddbshConnect`, `descriptor` → `Module.ddbshDescriptor` |
| `sdk-bridge.mjs` | DOM-free bridge: credentials (closure only), the `DynamoDBClient`, CONNECT validation + fail-closed blocking, the two middlewares, the SHOW CONNECTION descriptor, `installHooks(Module, bridge)` |
| `app-core.mjs` | DOM-free UI logic: prompt, transcript (ordered segments), history, run guard, `parseCredentials` / `validateCredentials` / `credentialStatus` |
| `app.js` | thin DOM wiring: load the module, wire the bridge, the credentials panel, the editor (textContent only) |
| `index.html`, `styles.css` | the page; the CSP `<meta>` between the `csp:begin`/`csp:end` markers is generated |
| `bundle-files.txt` | the publish allowlist: the 9 files the page loads plus the 2 SDK notices, 11 in all |
| `package.cmake` | the packaging step (target `ddbsh-web-package`): copies the allowlist into `dist/ddbsh-web-<version>/`, zips it, writes `SHA256SUMS` |
| `hosting/aws/` | `ddbsh-web-hosting.yaml` (private S3 bucket + CloudFront with Origin Access Control and security headers) and `deploy.sh` (the upload); see `docs/HOSTING.md` |
| `vendor/aws-sdk/` | the pinned SDK: `package.json` + `package-lock.json` + `entry.mjs` + `generate.mjs` (inputs), `aws-sdk-dynamodb.mjs` + `allowed-hosts.mjs` + `THIRD-PARTY-NOTICES.txt` + `PROVENANCE.json` (outputs) |
| `ddbsh-node.mjs` | CLI-shaped Node front end over the same files (the web golden gate's `DDBSH_SHELL_BIN`) |
| `live-smoke.mjs` | live UI-path smoke (`DdbshSession.run`) against real DynamoDB |
| `app-core.test.mjs`, `sdk-bridge.test.mjs` | offline Node unit tests (the bridge tests run the real SDK bundle with a stub HTTP handler) |
| `package.test.mjs` | offline check of the packaging step: the allowlist matches the page's load graph, the packaged folder, zip and `SHA256SUMS` match the allowlist and the build, and `package.cmake` refuses bad input |
| `package.json` | marks the bundle dir as ES modules for Node |

## Prerequisites

- **emsdk 3.1.61** (pinned; not vendored; `DDBSH_BUILD_WEB` defaults OFF so the
  normal build needs no Emscripten):

  ```bash
  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
  cd ~/emsdk
  # emsdk's scripts need python >= 3.10; set EMSDK_PYTHON if python3 is older.
  EMSDK_PYTHON=/usr/bin/python3.12 python3.12 emsdk.py install 3.1.61
  EMSDK_PYTHON=/usr/bin/python3.12 python3.12 emsdk.py activate 3.1.61
  ```

  The link flags in `CMakeLists.txt` (Asyncify, embind, ES6 `MODULARIZE`,
  `-sDYNAMIC_EXECUTION=0`) are known-good for 3.1.61; re-verify them on a bump.
- **Node.js 20+** for the tests, the gates and `devtools/vendor-aws-sdk-js`
  (SDK ≥ 3.950 requires it). emsdk's bundled Node (24.x) is picked up by the
  devtools scripts.

## Build and serve

```bash
emcmake cmake --preset wasm-release
cmake --build build-preset/wasm-release
devtools/serve-web 8080          # 127.0.0.1 only; open http://localhost:8080/
```

`build-preset/wasm-release/web/` is then the complete static bundle, plus the
Node shim, the tests and build files. Remote use goes through
`ssh -L 8080:localhost:8080 <host>` (the page needs a secure context for
credentials; `http://localhost` is one). To publish, package it:
`cmake --build build-preset/wasm-release --target ddbsh-web-package` writes
`build-preset/wasm-release/dist/ddbsh-web-<version>/` (only the allowlisted
files), a zip of it, and `SHA256SUMS`.

## The vendored SDK

`aws-sdk-dynamodb.mjs` is an esbuild bundle of `@aws-sdk/client-dynamodb`
(exact pin in `vendor/aws-sdk/package.json`, reproducible from the committed
lockfile) exporting `DynamoDBClient` and the 26 command classes the codec uses.
No CDN and no runtime fetch of any third-party script.

- Rebuild: `devtools/vendor-aws-sdk-js`.
- Verify (CI): `devtools/vendor-aws-sdk-js --check` — rebuilds in a temp dir and
  compares sha256s with `PROVENANCE.json` and the CSP in `index.html`.
- Bump: edit the pin, `devtools/vendor-aws-sdk-js --lock`, commit
  `web/vendor/aws-sdk/*` and `web/index.html`.

`allowed-hosts.mjs` is generated from the pinned SDK's partition data:
`REGIONS` (the `aws` and `aws-us-gov` partitions) and `ALLOWED_HOSTS`
(`[search-]dynamodb[-fips].<region>.{amazonaws.com,api.aws}`). The
`search-dynamodb` hosts are there because the SDK's endpoint rules send
`SearchVectors` (`SELECT … NEAREST`) to `search-dynamodb.<region>`; they are
not accepted as a `CONNECT … WITH ENDPOINT` override. It is the single source
for the bridge's endpoint check and the page CSP's `connect-src`.

## Content Security Policy

```
default-src 'none'; script-src 'self' 'wasm-unsafe-eval';
connect-src 'self' <every ALLOWED_HOSTS entry>; style-src 'self'; img-src 'self';
base-uri 'none'; form-action 'none'; object-src 'none';
require-trusted-types-for 'script'; trusted-types 'none'
```

`'wasm-unsafe-eval'` permits WebAssembly compilation only. The two Trusted
Types directives (P10) make every DOM injection sink (`innerHTML`, script URLs)
refuse plain strings and forbid creating a Trusted Types policy; the page
writes text with `textContent` only, and a headless Chromium run showed no
violation. A `<meta>` policy cannot set `frame-ancestors`; a host sends that
as a response header (`docs/HOSTING.md`). The glue is linked
with `-sDYNAMIC_EXECUTION=0` so it needs no `'unsafe-eval'`; the gates grep the
emitted `ddbsh.js` for `eval(` / `new Function`. `index.html` has no inline
script or style. `app-core.test.mjs` fails if the CSP drifts from
`ALLOWED_HOSTS`.

## Gates

| Command | What it proves |
|---|---|
| `devtools/run-web-smoke` | build and package; offline gates (no-eval glue, `app-core.test.mjs`, `sdk-bridge.test.mjs`, `package.test.mjs`, storage grep, upload commands of `deploy.sh` = `docs/HOSTING.md`); live `DdbshSession.run` smoke on one `ddbsh_web_smoke_<pid>` table |
| `devtools/check-web-hosting <stack>` | a deployed `hosting/aws` stack: status, content type, `Cache-Control`, security headers and hash of every file over HTTPS, the HTTP redirect, and that the bucket is private (`discussions/test-s3host-<hash>.log`) |
| `devtools/run-tests --web-golden` | the whole golden suite through `ddbsh-node.mjs` against real DynamoDB |

`run-web-smoke` and `run-tests --web-golden` mint short-term STS credentials;
`check-web-hosting` uses the caller's AWS CLI credentials only to read the
stack and the bucket's settings. See [`docs/TESTING.md`](../docs/TESTING.md).

## Boundaries

- The engine (`include/ddbsh/*.hpp`, `src/`) and `ddbsh-wire` stay SDK-free and
  terminal-free (`engine_boundary_check`). `<emscripten.h>` appears only in this
  directory's TUs and in the pre-ratified `#if __EMSCRIPTEN__` arm of
  `WireClient::backoff_sleep`.
- The CLI driver (`cli/repl.*`) is shared, not copied: `run` uses `run_batch`,
  `runScript` uses `run_source`.
- Credentials never cross into WASM memory, `Module`, `window`, storage, the URL
  or a log.
