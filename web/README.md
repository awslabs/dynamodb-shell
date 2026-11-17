# ddbsh web target (Emscripten/WASM) — P6d/P6e

This directory builds the browser/WASM target: the SDK-free ddbsh engine
compiled to WebAssembly, driven from JavaScript, talking to an in-browser
ExtendDB instance. No AWS SDK, no terminal, no network, no credentials — the
whole shell runs in-process against in-memory ExtendDB (in-memory SQLite).

P6d delivered the **target + bridge + embind surface + a Node smoke**. **P6e adds
the product UI** (`index.html`/`app.js`, history, file upload, result pane) — the
page a human opens in a browser and types into.

## What's here

| File | Role |
|------|------|
| `CMakeLists.txt` | the `ddbsh-web` target (engine + wire + cli-core → `ddbsh.wasm`/`ddbsh.js`) |
| `bindings.cpp` | the embind surface: `DdbshSession.run(text) -> RunResult{stdout,stderr,ok,status}` (WASM-only) |
| `transport_browser.cpp` | the Asyncify bridge: `SubmitTransport::send` → `EM_ASYNC_JS` → `await Module.submit(op,body)` (WASM-only) |
| `index.html` | the product page: connection panel, result pane, editor + toolbar (P6e) |
| `app.js` | thin DOM wiring: instantiate the modules, wire `Module.submit`, bind events, paint via `textContent` (P6e) |
| `app-core.mjs` | **DOM-free** logic: history ring, prompt string, transcript model, file→submission, run guard (P6e) |
| `app-core.test.mjs` | Node unit tests for `app-core.mjs` (zero deps, no DOM, no WASM) (P6e) |
| `styles.css` | minimal monospace styling (P6e) |
| `harness.mjs` | the P6d end-to-end **Node** smoke (not the product UI) |
| `package.json` | marks the build output as ES modules for Node |
| `vendor/extenddb/` | the pinned, vendored in-browser ExtendDB adapter (see `vendor/extenddb/PROVENANCE.md`) |

The engine's public header `include/ddbsh/wire/submit_transport.hpp` declares the
portable surface; only `transport_browser.cpp` (WASM-only) defines it.

## Architecture

```
  Browser page (P6d: harness; P6e: product UI)
    │  await ExtendDb.create();  Module.submit = (op,b) => db.submit(op,b);
    │  const s = new Module.DdbshSession("us-east-1");  s.run(text);
    ▼
  ddbsh.js (Emscripten ES6 glue)  +  ddbsh.wasm
    ├─ bindings.cpp (embind DdbshSession.run -> RunResult)
    ├─ engine (lex|parse|plan|execute|render)  — SDK-free, terminal-free
    ├─ ddbsh-wire (serialize/parse + WireClient)
    └─ transport_browser.cpp  SubmitTransport::send
          -> EM_ASYNC_JS ddbsh_wire_submit -> await Module.submit(op,body)  ◄─ Asyncify suspend
    ▼ (JS)
  extenddb_wasm_adapter.wasm (vendored; in-memory SQLite)
```

The engine's synchronous `IDDBClient` call path drives ExtendDB's async
`submit(op, body) -> Promise<string>` through a single `EM_ASYNC_JS` function.
`-sASYNCIFY` suspends and resumes the whole C++ stack on each `await`, so one
`IDDBClient` call that fans out to many backend calls (Query/Scan paging, the
table waiters, bulk UPDATE/DELETE) suspends per backend call transparently.

## Prerequisite — the Emscripten SDK (emsdk)

The web target needs the Emscripten toolchain. It is **not** vendored into the
repo and `DDBSH_BUILD_WEB` defaults **OFF**, so the normal CI matrix needs no
Emscripten. Install a pinned emsdk locally:

```bash
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk
# Pinned for P6d. emsdk's scripts need python >= 3.10; set EMSDK_PYTHON if the
# default python3 is older (this host's /usr/bin/python3 is 3.9):
EMSDK_PYTHON=/usr/bin/python3.12 python3.12 emsdk.py install 3.1.61
EMSDK_PYTHON=/usr/bin/python3.12 python3.12 emsdk.py activate 3.1.61
```

**Pinned version: Emscripten 3.1.61.** The link flags in `CMakeLists.txt`
(Asyncify + embind + ES6 `MODULARIZE`) are known-good for 3.1.x; re-verify them
on an emsdk bump.

> Toolchain note: `emcc` and the emsdk scripts require python ≥ 3.10. If the
> system `python3` is older, export `EMSDK_PYTHON=<python3.10+>` for every
> emsdk/emcc invocation (`devtools/run-web-smoke` auto-detects this).

## Build

Configure via `emcmake` so the Emscripten toolchain file is injected (do not
hard-code the toolchain path in the preset):

```bash
source ~/emsdk/emsdk_env.sh          # or set PATH + EMSDK_PYTHON directly
emcmake cmake --preset wasm-release   # DDBSH_BUILD_AWS=OFF, DDBSH_BUILD_WEB=ON, DDBSH_BUILD_TESTS=OFF
cmake --build build-preset/wasm-release
```

Output lands in `build-preset/wasm-release/web/`: `ddbsh.js`, `ddbsh.wasm`, and
(staged by a POST_BUILD step) `vendor/`, `harness.mjs`, `package.json`, and the
P6e UI bundle (`index.html`, `app.js`, `app-core.mjs`, `app-core.test.mjs`,
`styles.css`). After `cmake --build`, that directory is a **complete,
directly-servable static bundle** — nothing to assemble by hand.

## Run the UI (serve over HTTP)

WASM + ES modules **cannot load from `file://`** (module CORS + the
`application/wasm` MIME requirement), so the bundle must be served over HTTP. The
app shows a one-line banner and refuses to start if opened from `file://`.

```bash
devtools/serve-web            # builds the preset if needed, then serves :8080
# or, by hand, from the build output dir:
cd build-preset/wasm-release/web && python3 -m http.server 8080
```

Then open <http://localhost:8080/>. No cross-origin-isolation (COOP/COEP) headers
are required: the target uses Asyncify, not threads/SharedArrayBuffer, so plain
`http.server` is enough. (A future threaded build would need COOP/COEP.)

Using the UI:

- **Editor.** Type a statement (or several `;`-separated statements) and press
  **Ctrl/⌘-Enter** (or click **Run**). Plain **Enter** inserts a newline, so
  multi-line statements are normal. **ArrowUp/ArrowDown** recall history when the
  caret is on the first/last line of the editor.
- **History** persists across reloads in `localStorage` (`ddbsh.history`, a capped
  ring of 500 entries).
- **File upload** (⇪ file) runs an uploaded file's entire contents as one
  submission — the browser equivalent of the CLI's `-f`/`source`. The file is not
  added to the command history.
- **Clear** empties the transcript pane (not the history).
- There is **no `~/.ddbsh_config`** in the browser; the session starts in
  `us-east-1` against the in-browser ExtendDB backend.

## Smoke (Node, no browser)

```bash
devtools/run-web-smoke          # builds the preset + runs harness.mjs + app-core tests, writes an artifact
# or, by hand:
cd build-preset/wasm-release/web && node harness.mjs && node app-core.test.mjs
```

The smoke instantiates the module, wires `Module.submit` to the vendored
ExtendDB adapter, and runs a full op-family sequence (CREATE/INSERT/GetItem/
Query/EXPLAIN/UPDATE/DELETE/SHOW LIMITS/DROP) plus two error-degradation paths
(a conditional-check failure and an absent table), asserting the rendered
output. `run-web-smoke` then runs `app-core.test.mjs` — the DOM-free P6e UI unit
tests (history ring, prompt builder, transcript model, file→submission, run
guard), which need no WASM or browser. A GUI browser hand-test of `index.html`
is the P6e human acceptance step (there is no headless-browser test in the
default gate; see the P6e design discussion).

> Node note: a browser defines `globalThis.crypto` natively; Node 18 does not in
> the ESM scope, so `harness.mjs` installs Node's Web Crypto before the adapter
> runs. That is the only node-vs-browser shim.

## Supported / unsupported ops in the browser (degradation map)

The in-browser ExtendDB adapter answers most of the DynamoDB surface ddbsh
emits. Where it does not, the engine surfaces ExtendDB's `__type` error cleanly
through the normal error renderer — **a documented backend limitation, not a
ddbsh bug**. If you run `BACKUP TABLE …` or `ALTER … ADD REPLICA …` and see an
error, this table is why. (The real-DDB golden suite, run via
`devtools/run-tests`, exercises the full surface and is the regression contract;
nothing here weakens it.)

| ddbsh command family | Operation(s) | In-browser ExtendDB |
|---|---|---|
| SELECT (GetItem/Query/Scan) | GetItem, Query, Scan | **Supported** |
| SELECT … NEAREST (vector) | SearchVectors | **Supported** |
| INSERT / REPLACE | PutItem | **Supported** |
| UPDATE / UPSERT | UpdateItem (+ Query/Scan for bulk) | **Supported** |
| DELETE | DeleteItem (+ Query/Scan for bulk) | **Supported** |
| CREATE TABLE / DROP TABLE | CreateTable, DeleteTable | **Supported** |
| DESCRIBE / SHOW TABLES | DescribeTable, DescribeTimeToLive, DescribeContinuousBackups, DescribeTableReplicaAutoScaling | **Mostly**: DescribeTable/TTL/ContinuousBackups yes; replica-autoscaling likely errors (global tables unsupported) → that section is skipped, as the engine already tolerates a Describe failure there |
| SHOW LIMITS | DescribeLimits (+ ListTables/DescribeTable) | **Supported** |
| ALTER TABLE SET … | UpdateTable, UpdateTimeToLive, UpdateContinuousBackups | **Supported** (data/TTL/PITR state) |
| ALTER TABLE … REPLICA | UpdateTable (ReplicationGroupUpdate) | **Unsupported** (global tables) → clean `__type` error |
| BACKUP / DROP BACKUP / DESCRIBE BACKUP / RESTORE / SHOW BACKUPS | CreateBackup/DeleteBackup/DescribeBackup/RestoreTable*/ListBackups | **Unsupported in the port** (adapter stubs backup/restore) → clean out-of-scope error |
| transactions | TransactGetItems, TransactWriteItems | **Supported** |
| CONNECT | (client reconfigure) | **No-op** in-browser (single fixed backend); refreshes the region label only |

## Boundaries (unchanged invariants)

- The engine (`include/ddbsh/*.hpp`, `src/`) and `ddbsh-wire` stay SDK-free and
  terminal-free (`engine_boundary_check`). `<emscripten.h>` appears only in
  `transport_browser.cpp` (the WASM target, not `ddbsh-wire`) and in the single
  pre-ratified `#if __EMSCRIPTEN__` arm of `WireClient::poll_delay`.
- The batch-execution policy is shared with the CLI via
  `ddbsh::cli::run_batch` (`cli/repl.*`) — no duplicated, drift-prone copy.
- The CLI and the real-DDB golden suite are untouched; the golden contract
  stays green.
