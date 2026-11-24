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
| `bindings.cpp` | embind `DdbshSession`: `runStreaming(text, options, onEvent)` (the UI path, `run_batch`: each piece of output goes to `onEvent` as an event as soon as the engine emits it; `options` is `{output, jsonFlavor}`, plus the web-only output `html`; see [Streaming and output formats](#streaming-and-output-formats)), `run(text)` (the same path, buffered), `runScript(text, abortOnError, options)` (CLI `-c`/`-f` path, `run_source`; `options` is `{output, jsonFlavor}`, the CLI's `--output`/`--json-flavor`), `region()`, `endpointOverridden()`; `RunResult {stdout, stderr, segments, ok, status}`; `WebWireClient` (retry cap 1, CONNECT-rejection notice); the free function `version()` (the header's version) |
| `js_sdk_transport.{hpp,cpp}` | `JsSdkTransport`: `send` → `Module.ddbshSend` (Asyncify), `on_connect` → `Module.ddbshConnect`, `descriptor` → `Module.ddbshDescriptor` |
| `sdk-bridge.mjs` | DOM-free bridge: credentials (closure only), the `DynamoDBClient`, CONNECT validation + fail-closed blocking, the two middlewares, the retry-strategy wrapper behind `onRetryEvent` (a `retry` event for each retry the SDK decides to make, before its backoff and with that backoff's delay, and `settled` when each send ends; SDK 3.1174.0 sleeps inside its retry strategy, so the wrapper hooks the strategy's backoff computation to raise the event before the sleep, and falls back to raising it after the strategy returns when there is no such hook), the SHOW CONNECTION descriptor, `installHooks(Module, bridge)` |
| `app-core.mjs` | DOM-free UI logic: prompt, transcript (ordered segments; the streaming `createTranscriptAppender`, `guardedEventSink` and `streamSubmission`), the HTML result table's column growth (`createTableModel`), the output dropdowns (`OUTPUT_CHOICES`, `JSON_FLAVOR_CHOICES`, `outputOptions`), history, run guard, `parseCredentials` / `validateCredentials` / `credentialStatus`, the throttling-code list (`isThrottlingError`), the header's `versionLabel` and busy clock (`createBusyClock`, with its throttled / retrying states) |
| `app.js` | thin DOM wiring: load the module, wire the bridge, the credentials panel, the header (version, busy indicator and its retry states), the editor, the Output and JSON dropdowns, the streaming transcript (Text nodes and textContent only) and the HTML result tables (`createElement` and `textContent`, classes only) |
| `index.html`, `styles.css` | the page; the CSP `<meta>` between the `csp:begin`/`csp:end` markers is generated |
| `bundle-files.txt` | the publish allowlist: the 9 files the page loads plus the 2 SDK notices, 11 in all |
| `package.cmake` | the packaging step (target `ddbsh-web-package`): copies the allowlist into `dist/ddbsh-web-<version>/`, zips it, writes `SHA256SUMS` |
| `hosting/aws/` | `ddbsh-web-hosting.yaml` (private S3 bucket + CloudFront with Origin Access Control and security headers) and `deploy.sh` (the upload); see `docs/HOSTING.md` |
| `vendor/aws-sdk/` | the pinned SDK: `package.json` + `package-lock.json` + `entry.mjs` + `generate.mjs` (inputs), `aws-sdk-dynamodb.mjs` + `allowed-hosts.mjs` + `THIRD-PARTY-NOTICES.txt` + `PROVENANCE.json` (outputs) |
| `ddbsh-node.mjs` | CLI-shaped Node front end over the same files (the web golden gate's `DDBSH_SHELL_BIN`) |
| `live-smoke.mjs` | live UI-path smoke (`DdbshSession.run`, and `runStreaming` in the default, `html` and `json` outputs) against real DynamoDB |
| `app-core.test.mjs`, `sdk-bridge.test.mjs` | offline Node unit tests (the bridge tests run the real SDK bundle with a stub HTTP handler) |
| `streaming.test.mjs` | offline Node test of `runStreaming` against the built `ddbsh.wasm` with stub transport hooks: SHOW TABLES row k reaches the callback before row k+1 is described, the events equal `runScript`'s and `run`'s segments (in every format and flavor), the `html` rows/end events (column growth, cells, empty results, a mid-stream error), option errors, and a throwing callback does not break the run |
| `package.test.mjs` | offline check of the packaging step: the allowlist matches the page's load graph, the packaged folder, zip and `SHA256SUMS` match the allowlist and the build, and `package.cmake` refuses bad input |
| `package.json` | marks the bundle dir as ES modules for Node |

## Streaming and output formats

The page runs every submission through
`DdbshSession.runStreaming(text, options, onEvent)` (P12, D-P12-17). It runs
`text` as one `run_batch` submission and calls `onEvent` synchronously each
time the engine emits a piece of output, so the page paints it while the next
DynamoDB request is in flight. It returns `{ok, status}` (a `RunResult` with
empty `stdout`, `stderr` and `segments`, so nothing is delivered twice).

`options` selects the output (D-P12-26). An absent field keeps the default.

| Field | Values |
|---|---|
| `output` | `default`, `json`, `csv`, `table` (as the CLI's `--output`), or `html` (web only) |
| `jsonFlavor` | `plain` or `dynamodb` (as the CLI's `--json-flavor`); ignored by `default` |

An unknown value runs nothing: `onEvent` receives the CLI's message as one
`stderr` text event, and the status is 1. `runScript` takes the same options
but rejects `html`, as the CLI does.

The events:

- `{type: "text", stream: "stdout" | "stderr", text}`: one per piece of
  rendered output, in the CLI's order. A failing statement's error is a
  `stderr` event after that statement's output. In `default`, `json`, `csv`
  and `table` every event is a text event, and the events of a run
  concatenate to the bytes `runScript` returns for the same text and options.
- `{type: "rows", columns, newColumnsFrom, rows, groupEnd}`: `html` only (D-P12-25).
  Rows of the open result set. `columns` is the set's full column list so far
  (it only grows); `newColumnsFrom` is the index of the first column this
  event added (equal to `columns.length` when it added none). `rows` holds one
  array per row with one cell per column: `{t, n, m}`, where `t` is the cell's
  text (not escaped: set it with `textContent`), `n` is true for a number,
  and `m` is true when the row has no such attribute. `groupEnd` is true when
  a DynamoDB response page ended with this event. The first event of a set
  always comes, so an empty result still carries its columns; a later event
  comes only when it brings rows or columns.
- `{type: "end"}`: `html` only. The open result set closed. It comes before
  the text that follows the set and before a failing statement's error.

In `html` everything that is not rows (status tokens, DESCRIBE, EXPLAIN, the
`SELECT (…)` capacity line) is a text event with the same bytes as in
`default`. The library emits no HTML: the page builds each result set as a
`<table>` with `createElement` and `textContent`, styled by class only, because
the CSP below allows no `innerHTML` and no `style` attribute. When an event
adds columns, the page appends a header cell and an empty cell to every row
already in the table (`createTableModel` in `app-core.mjs`).

The result sets of `html` open and close exactly where `json`'s arrays do:
both sinks derive from the library's `ddbsh::render::ResultSetSink`, which
holds the one open-set rule (a row part opens a set or continues it; text, or
the end of the statement, closes it).

The Output dropdown (Default, JSON, CSV, Table (text), Table (HTML)) and the
JSON dropdown (Plain, DynamoDB; disabled for Default) sit by the Run button.
The page reads them at submit and does not save them.

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
  (SDK ≥ 3.950 requires it). The devtools scripts use the `node` on `PATH` when
  it is 20 or newer, and otherwise emsdk's newest bundled Node (24.x in a
  current emsdk checkout).

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
| `devtools/run-web-smoke` | build and package; the offline gates (`web_offline_gates` in `devtools/lib-web.sh`: no-eval glue, `app-core.test.mjs`, `sdk-bridge.test.mjs`, `streaming.test.mjs`, `package.test.mjs`, storage grep, upload commands of `deploy.sh` = `docs/HOSTING.md`; CI runs the same set); live `DdbshSession.run` and `runStreaming` smoke on one `ddbsh_web_smoke_<pid>` table |
| `devtools/check-web-hosting <stack>` | a deployed `hosting/aws` stack: status, content type, `Cache-Control`, security headers and hash of every file over HTTPS, the HTTP redirect, and that the bucket is private (`discussions/test-s3host-<hash>.log`) |
| `devtools/check-web-hosting --anonymous <url>` | the same checks of the served files by the site's address, with no credentials; `--bucket-domain` adds the anonymous S3 refusals and `--wait` first waits for the site to serve the bundle (the site publish workflow runs it) |
| `devtools/run-tests --web-golden` | the offline gates, then the whole golden suite through `ddbsh-node.mjs` against real DynamoDB |

`run-web-smoke` and `run-tests --web-golden` mint short-term STS credentials;
`check-web-hosting` uses the caller's AWS CLI credentials only to read the
stack and the bucket's settings, and none with `--anonymous`. See
[`docs/TESTING.md`](../docs/TESTING.md).

## Boundaries

- The engine (`include/ddbsh/*.hpp`, `src/`) and `ddbsh-wire` stay SDK-free and
  terminal-free (`engine_boundary_check`). `<emscripten.h>` appears only in this
  directory's TUs and in the pre-ratified `#if __EMSCRIPTEN__` arm of
  `WireClient::backoff_sleep`.
- The CLI driver (`cli/repl.*`) is shared, not copied: `runStreaming` and `run`
  use `run_batch`, `runScript` uses `run_source`.
- `runStreaming`'s callback runs while the engine is on the stack. It must not
  throw (a JavaScript exception unwinds the WebAssembly frames without C++
  cleanup) and must not call into the module or the session; the page passes
  it through `guardedEventSink` and touches only the transcript.
- Credentials never cross into WASM memory, `Module`, `window`, storage, the URL
  or a log.
