# ddbsh web target (Emscripten/WASM) — P6d

This directory builds the browser/WASM target: the SDK-free ddbsh engine
compiled to WebAssembly, driven from JavaScript, talking to an in-browser
ExtendDB instance. No AWS SDK, no terminal, no network, no credentials — the
whole shell runs in-process against in-memory ExtendDB (in-memory SQLite).

P6d delivers the **target + bridge + embind surface + a Node smoke**. The
product UI (`index.html`/`app.js`, history, file upload, result pane) is **P6e**.

## What's here

| File | Role |
|------|------|
| `CMakeLists.txt` | the `ddbsh-web` target (engine + wire + cli-core → `ddbsh.wasm`/`ddbsh.js`) |
| `bindings.cpp` | the embind surface: `DdbshSession.run(text) -> RunResult{stdout,stderr,ok,status}` (WASM-only) |
| `transport_browser.cpp` | the Asyncify bridge: `SubmitTransport::send` → `EM_ASYNC_JS` → `await Module.submit(op,body)` (WASM-only) |
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
(staged by a POST_BUILD step) `vendor/`, `harness.mjs`, `package.json`.

## Smoke (Node, no browser)

```bash
devtools/run-web-smoke          # builds the preset + runs harness.mjs, writes an artifact
# or, by hand:
cd build-preset/wasm-release/web && node harness.mjs
```

The smoke instantiates the module, wires `Module.submit` to the vendored
ExtendDB adapter, and runs a full op-family sequence (CREATE/INSERT/GetItem/
Query/EXPLAIN/UPDATE/DELETE/SHOW LIMITS/DROP) plus two error-degradation paths
(a conditional-check failure and an absent table), asserting the rendered
output. It runs under Node (the module loads identically in a browser); a GUI
browser hand-test is a P6e concern.

> Node note: a browser defines `globalThis.crypto` natively; Node 18 does not in
> the ESM scope, so `harness.mjs` installs Node's Web Crypto before the adapter
> runs. That is the only node-vs-browser shim.

## Boundaries (unchanged invariants)

- The engine (`include/ddbsh/*.hpp`, `src/`) and `ddbsh-wire` stay SDK-free and
  terminal-free (`engine_boundary_check`). `<emscripten.h>` appears only in
  `transport_browser.cpp` (the WASM target, not `ddbsh-wire`) and in the single
  pre-ratified `#if __EMSCRIPTEN__` arm of `WireClient::poll_delay`.
- The batch-execution policy is shared with the CLI via
  `ddbsh::cli::run_batch` (`cli/repl.*`) — no duplicated, drift-prone copy.
- The CLI and the real-DDB golden suite are untouched; the golden contract
  stays green.
