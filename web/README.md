# ddbsh web target (Emscripten/WASM) — P6d/P6e/P6f

This directory builds the browser/WASM target: the SDK-free ddbsh engine
compiled to WebAssembly, driven from JavaScript. The whole hard-to-port stack
(lexer, parser, planner, executor, renderer, `ddbsh-wire` codec, `WireClient`)
ships as **one portable WASM artifact**; only the linked wire `Transport` and the
backend wiring change between editions. No AWS SDK, no terminal, and no
credentials are ever in the browser.

There are **three setups** (design §7). Two use the native **`ddbsh-web-proxy`**
(it signs with SigV4 and forwards; credentials stay on the host, never in the
browser); the third runs an in-browser database with no server at all:

1. **Real Amazon DynamoDB**, via the fetch edition + `ddbsh-web-proxy`. See
   "Real backend via ddbsh-web-proxy" below. **Production-ready.**
2. **Real local ExtendDB server** (SQLite, HTTPS), via the fetch edition + the
   same proxy. See "Local ExtendDB via the proxy" below. **Production-ready.**
3. **In-browser ExtendDB** (in-memory SQLite), via the submit edition — no
   server, no network, no credentials, no TLS; safe to host publicly. **This
   edition is pre-release**: it depends on the ExtendDB WebAssembly adapter,
   which ddbsh does not ship (a drop-in; see "In-browser ExtendDB edition"
   below). It is complete when an official ExtendDB WebAssembly release exists.
   Being the zero-install, trivially cross-platform edition, it is also the
   **Windows baseline** the maintainer hand-verifies (design §11).

P6d delivered the **target + bridge + embind surface + a Node smoke**; P6e added
the **product UI** (`index.html`/`app.js`, history, file upload, result pane);
P6f added the **real-backend** path (the fetch edition, the proxy, and the local
ExtendDB server wiring).

## What's here

| File | Role |
|------|------|
| `CMakeLists.txt` | the two WASM editions (shared engine+wire+cli-core; one links `SubmitTransport`, one links `FetchTransport` — see "Two editions" below) |
| `bindings.cpp` | the embind surface: `DdbshSession.run(text) -> RunResult{stdout,stderr,ok,status}`; picks the transport by `DDBSH_WEB_FETCH` (WASM-only) |
| `transport_browser.cpp` | the Asyncify bridge: `SubmitTransport::send` → `EM_ASYNC_JS` → `await Module.submit(op,body)` (in-tab edition, WASM-only) |
| `transport_fetch.cpp` | the Asyncify bridge: `FetchTransport::send`/`on_connect` → `EM_ASYNC_JS` → `await Module.fetchDdb/connectDdb` (real-backend/proxy edition, WASM-only) |
| `index.html` | the product page: connection panel, result pane, editor + toolbar (P6e) |
| `app.js` | thin DOM wiring: auto-select the edition (probe the proxy `/healthz`), instantiate the module, wire `Module.submit` (in-tab) or `Module.fetchDdb`/`connectDdb` + the proxy handshake (fetch), bind events, paint via `textContent` (P6e/P6f-e) |
| `app-core.mjs` | **DOM-free** logic: history ring, prompt string, transcript model, file→submission, run guard (P6e) |
| `app-core.test.mjs` | Node unit tests for `app-core.mjs` (zero deps, no DOM, no WASM) (P6e) |
| `styles.css` | minimal monospace styling (P6e) |
| `harness.mjs` | the P6d end-to-end **Node** smoke, in-tab edition (not the product UI) |
| `harness-fetch.mjs` | the P6f-a end-to-end **Node** smoke, fetch edition over a stubbed proxy |
| `harness-proxy.mjs` | the P6f-b end-to-end **Node** smoke, fetch edition over the **real** `ddbsh-web-proxy` against **real DynamoDB** (run by `devtools/run-proxy-smoke`) |
| `harness-extenddb.mjs` | the P6f-c end-to-end **Node** smoke, fetch edition over the **real** `ddbsh-web-proxy` against a **real local ExtendDB** server, HTTPS (run by `devtools/run-extenddb-smoke`) |
| `harness-proxy-lib.mjs` | the shared browser-handshake + op-family corpus both proxy smokes drive (one corpus, two backends) |
| `package.json` | marks the build output as ES modules for Node |
| `vendor/extenddb/` | **drop-in location** for the in-browser ExtendDB adapter — ddbsh does **not** ship/commit the adapter blob (D-P6f-8); only `PROVENANCE.md` + `.gitignore` are tracked (see `vendor/extenddb/PROVENANCE.md`) |

The engine's public headers `include/ddbsh/wire/submit_transport.hpp` and
`include/ddbsh/wire/fetch_transport.hpp` declare the portable surfaces; only
`transport_browser.cpp` / `transport_fetch.cpp` (WASM-only) define them.

## Two editions (P6f-a)

One source tree, two Emscripten build targets. They share the whole engine, the
`ddbsh-wire` codec, the WireClient, the embind surface, and the UI; they differ
only by the linked wire `Transport` (D-P6f-2):

| Target | Transport | JS hook | Output | Backend |
|--------|-----------|---------|--------|---------|
| `ddbsh-wasm` | `FetchTransport` | `Module.fetchDdb(op,body)` + `Module.connectDdb(region,endpoint)` | `ddbsh.js`/`ddbsh.wasm` | `ddbsh-web-proxy` → real DynamoDB / real ExtendDB (proxy signs; the browser never holds credentials) — the **primary** production edition |
| `ddbsh-wasm-extenddb-wasm` | `SubmitTransport` | `Module.submit(op,body)` | `ddbsh-extenddb.js`/`ddbsh-extenddb.wasm` | in-tab ExtendDB (no server, no credentials, no network) — **pre-release** |

`SHOW CONNECTION` reports the running composition (library version, region /
endpoint, and the transport/backend binding) so you can tell which edition and
backend a page is driving. The in-tab edition reports
`wire/submit -> in-tab ExtendDB`; the proxy edition reports
`wire/fetch -> POST /ddb (proxy)`, enriched after the handshake/CONNECT with the
live proxy version and the connected endpoint, e.g.
`wire/fetch -> POST /ddb (proxy 0.11.14 -> https://127.0.0.1:18443)` (D-P6f-9).

The `ddbsh` vs `ddbsh-extenddb` output-filename convention (ratified P6f-e):
the production proxy edition owns the clean `ddbsh` name; the in-browser ExtendDB
edition is suffixed. The product UI (`app.js`) loads whichever edition fits how
the page is served — see "Run the UI" — so one `index.html` bundle drives both.

## Real backend via ddbsh-web-proxy (P6f-b)

The fetch edition (`ddbsh-wasm`) holds **no credentials** and cannot call
DynamoDB directly (no CORS). The native **`ddbsh-web-proxy`** (in `proxy/`, a Rust
single binary) is the companion that makes the real path work: it serves the
static bundle, holds per-session connection state, and on `POST /ddb` SigV4-signs
the browser's AWS-JSON-1.0 body with credentials resolved from the host's AWS
chain and forwards it to the session's DynamoDB / ExtendDB endpoint. The response
(including a DynamoDB `__type` error document) is relayed back for the
`ddbsh-wire` codec to map. Credentials never reach the browser.

The JS glue the fetch edition expects:

- `Module.fetchDdb(op, body) -> Promise<string>` — POST `body` to the proxy's
  `/ddb` with headers `content-type: application/x-amz-json-1.0`,
  `x-ddbsh-op: <op>`, `x-ddbsh-session`, `x-ddbsh-token`. Resolve with the
  response body on 2xx or a DynamoDB `__type` error; reject on a transport
  failure.
- `Module.connectDdb(region, endpoint) -> Promise<string>` — POST
  `{region, endpoint?}` to `/connect` (same session headers). Resolve `""` on
  success, an error message otherwise. The engine's `CONNECT` statement fires
  this through `FetchTransport::on_connect` so the proxy re-targets.

Before either, the page does the one-time handshake: `POST /session` (same
origin) mints `{session, token}`. See `harness-proxy.mjs` for the reference
implementation (the browser UI wiring is P6f-e).

## Local ExtendDB via the proxy (P6f-c)

The same fetch edition + the same proxy also drive a **real local ExtendDB**
server (a DynamoDB-wire-compatible server you run yourself), selected by the
`CONNECT` endpoint:

```
connect us-east-1 with endpoint "https://127.0.0.1:18443";
```

ExtendDB differs from AWS in two ways the proxy handles **per endpoint**
(D-P6f-7), both supplied on the `/connect` body — neither is a secret:

- **Credentials.** ExtendDB issues its **own** SigV4 access key (it does not
  accept real AWS keys). You store that key in a named AWS profile and the glue
  sends the proxy a `profile` field naming it; the proxy resolves the key from
  that profile. The actual key stays in the credentials file the proxy reads —
  it never reaches the browser.
- **TLS trust.** ExtendDB is **HTTPS-only** with a **self-signed cert**
  (`~/.extenddb/tls/cert.pem`); the glue sends a `ca_bundle` field with that
  cert's path so the proxy trusts it. Plain HTTP is never used.

So the fetch edition's `connectDdb` glue, for an ExtendDB endpoint, POSTs
`{region, endpoint, profile, ca_bundle}` to `/connect` (vs `{region}` for AWS).
`harness-extenddb.mjs` is the reference; the product UI's per-endpoint wiring is
P6f-e.

Human setup (one-time), mirroring ExtendDB's own getting-started:

```bash
# 1. Build + init a local ExtendDB (SQLite backend; HTTPS + self-signed cert).
cargo build --release                     # in your ExtendDB checkout
./target/release/extenddb init --backend sqlite    # prints the admin password once

# 2. Issue a DynamoDB key for ddbsh's proxy to use.
read -rs EXTENDDB_PASSWORD && export EXTENDDB_PASSWORD      # the admin password
ACCT=$(./target/release/extenddb manage --user admin list-accounts \
        | sed -n 's/.*"account_id":[[:space:]]*"\([0-9]*\)".*/\1/p' | head -1)
./target/release/extenddb manage --user admin create-user --account-id "$ACCT" --user-name ddbsh
./target/release/extenddb manage --user admin put-user-policy --account-id "$ACCT" \
    --user-name ddbsh --policy-name FullAccess \
    --policy-document '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"dynamodb:*","Resource":"*"}]}'
./target/release/extenddb manage --user admin create-access-key --account-id "$ACCT" --user-name ddbsh
unset EXTENDDB_PASSWORD

# 3. Put the issued key in a dedicated ~/.aws profile (NOT your AWS keys).
cat >> ~/.aws/credentials <<'EOF'
[extenddb]
aws_access_key_id = AKIAEXTENDDB...
aws_secret_access_key = extenddb...
EOF

# 4. Start the server and the proxy; CONNECT with the endpoint in the UI.
./target/release/extenddb serve --config extenddb.toml
ddbsh-web-proxy --serve --webroot build-preset/wasm-release/web
```

The automated gate (`devtools/run-extenddb-smoke`) does all of this
hermetically — it inits a throwaway SQLite deployment, issues a key into a
**temporary** credentials file (your real `~/.aws` is never touched), launches
ExtendDB + the proxy on loopback, runs `harness-extenddb.mjs`, and tears
everything down.


**Security (builder-security, followed as policy; see `proxy/README.md`):** the
proxy binds **loopback only** and is **not configurable** to bind wider (remote
access is an `ssh -L` tunnel); an **origin + per-session-token** guard stops a
malicious same-browser page from driving it; a **user-owned egress policy**
(`~/.ddbsh_web_proxy_security`, anchored allow/block regexes, block wins; default
= loopback + DynamoDB host families, IMDS blocked) bounds where the credentialed
relay may be pointed.

## Architecture

```
  Browser page (P6e/P6f-e product UI; app.js auto-selects the edition)
    │  in-tab:  await ExtendDb.create(); Module.submit = (op,b) => db.submit(op,b);
    │  proxy:   /session handshake; Module.fetchDdb/connectDdb POST to the proxy;
    │  const s = new Module.DdbshSession("us-east-1");  s.run(text);
    ▼
  ddbsh-extenddb.js / ddbsh.js (Emscripten ES6 glue)  +  the matching .wasm
    ├─ bindings.cpp (embind DdbshSession.run -> RunResult)
    ├─ engine (lex|parse|plan|execute|render)  — SDK-free, terminal-free
    ├─ ddbsh-wire (serialize/parse + WireClient)
    └─ transport (one per edition):
         in-tab: transport_browser.cpp  SubmitTransport::send
                 -> EM_ASYNC_JS -> await Module.submit(op,body)       ◄─ Asyncify suspend
         proxy:  transport_fetch.cpp    FetchTransport::send/on_connect
                 -> EM_ASYNC_JS -> await Module.fetchDdb/connectDdb    ◄─ Asyncify suspend
    ▼ (JS)
  in-tab: extenddb_wasm_adapter.wasm (drop-in; in-memory SQLite)
  proxy:  fetch() -> ddbsh-web-proxy (SigV4) -> real DynamoDB / real ExtendDB
```

The engine's synchronous `IDDBClient` call path drives the backend's async
`submit`/`fetch` through a single `EM_ASYNC_JS` function. `-sASYNCIFY` suspends
and resumes the whole C++ stack on each `await`, so one `IDDBClient` call that
fans out to many backend calls (Query/Scan paging, the table waiters, bulk
UPDATE/DELETE) suspends per backend call transparently.

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

Output lands in `build-preset/wasm-release/web/`: `ddbsh.js`, `ddbsh.wasm` (the
fetch/proxy edition), `ddbsh-extenddb.js`, `ddbsh-extenddb.wasm` (the in-tab
edition), and (staged by a POST_BUILD step) ddbsh's own
`harness.mjs`, `harness-fetch.mjs`, `package.json`, and the
P6e UI bundle (`index.html`, `app.js`, `app-core.mjs`, `app-core.test.mjs`,
`styles.css`). The in-browser ExtendDB adapter (`vendor/extenddb/`) is **not**
staged by the build (D-P6f-8) — it is a drop-in (see "In-browser ExtendDB
edition"); `devtools/run-web-smoke` and `devtools/serve-web` stage it
automatically. After `cmake --build` plus that drop-in, the directory is a
**complete, directly-servable static bundle**.

## Run the UI (serve over HTTP)

WASM + ES modules **cannot load from `file://`** (module CORS + the
`application/wasm` MIME requirement), so the bundle must be served over HTTP. The
app shows a one-line banner and refuses to start if opened from `file://`.

The one `index.html` bundle drives **both** editions; `app.js` auto-selects at
startup by probing the companion proxy's `/healthz`:

- **Served by `ddbsh-web-proxy`** → the probe succeeds → the **fetch edition**
  (`ddbsh.js`) loads, does the `/session` handshake, wires
  `Module.fetchDdb`/`Module.connectDdb` to the proxy, and talks to the **real**
  backend. This is the production path.

  ```bash
  ddbsh-web-proxy --serve --webroot build-preset/wasm-release/web   # then open http://localhost:8456/
  ```

- **Served by a plain static server** → the probe fails → the **in-tab edition**
  (`ddbsh-extenddb.js`) loads against in-browser ExtendDB (pre-release; no
  server, no credentials).

  ```bash
  devtools/serve-web            # builds the preset if needed, then serves :8080 (loopback)
  # or, by hand, from the build output dir:
  cd build-preset/wasm-release/web && python3 -m http.server --bind 127.0.0.1 8080
  ```

Then open the printed URL. No cross-origin-isolation (COOP/COEP) headers are
required: the target uses Asyncify, not threads/SharedArrayBuffer, so plain
`http.server` (or the proxy's static serving) is enough. (A future threaded build
would need COOP/COEP.)

`devtools/serve-web` binds **loopback (127.0.0.1) only** by default, so the bundle
is reachable only from this machine. To hand-test from another machine, prefer an
SSH local port-forward (`ssh -L 8080:localhost:8080 host`). If you knowingly want
the server reachable on your LAN, set the bind explicitly —
`DDBSH_SERVE_BIND=0.0.0.0 devtools/serve-web` — which exposes the (credential-free)
in-browser bundle to every host that can route to this interface. (The proxy
edition is never served this way: `ddbsh-web-proxy` binds loopback only and is
not configurable.)

**Pointing the proxy edition at a local ExtendDB.** A `CONNECT <region> WITH
ENDPOINT "<url>"` to a non-AWS endpoint needs the per-endpoint credential wiring
(a named AWS profile holding ExtendDB's issued key + a CA bundle for its
self-signed cert — see "Local ExtendDB via the proxy"). Supply them on the page
URL as query parameters, which `app.js` attaches to endpoint-override CONNECTs:

```
http://localhost:8456/?profile=extenddb&ca_bundle=/home/you/.extenddb/tls/cert.pem
```

Neither is a secret (a profile name + a file path); the proxy resolves the actual
key from that `~/.aws` profile and it never reaches the browser. A plain real-AWS
`CONNECT <region>` needs no parameters (the proxy signs with the default chain).

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
  `us-east-1`. In the in-tab edition that is the in-browser ExtendDB backend; in
  the proxy edition it is real AWS DynamoDB (`us-east-1`) via the signing proxy,
  and `CONNECT` re-targets the proxy session.

## Smoke (Node, no browser)

```bash
devtools/run-web-smoke          # builds both editions + runs harness.mjs, harness-fetch.mjs, and app-core tests, writes an artifact
# or, by hand:
cd build-preset/wasm-release/web && node harness.mjs && node harness-fetch.mjs && node app-core.test.mjs
```

`run-web-smoke` is **offline** (no credentials, no network): in-tab ExtendDB and
the stubbed-proxy fetch path. The **real-backend** gate is separate:

```bash
devtools/run-proxy-smoke        # builds the proxy + fetch wasm, launches the proxy on loopback,
                                # runs harness-proxy.mjs against REAL DynamoDB, writes an artifact
```

`run-proxy-smoke` talks to a **real AWS account** via the host's AWS chain: it
creates and drops one `ddbsh_web_proxy_*`-prefixed on-demand table and cleans it
up. A missing credential is an infrastructure failure, not a skip.

The **local-ExtendDB** real path has its own gate (P6f-c):

```bash
devtools/run-extenddb-smoke     # builds the proxy + fetch wasm, inits + launches a throwaway
                                # local ExtendDB (SQLite, HTTPS), issues a key into a TEMP creds
                                # file, launches the proxy, runs harness-extenddb.mjs, writes an artifact
```

`run-extenddb-smoke` is fully **local and loopback** — no real AWS account, no
network beyond localhost. It needs an ExtendDB SQLite binary (set
`DDBSH_EXTENDDB_BIN`, default `~/source/extenddb-READ-ONLY/target/release/extenddb`);
a missing binary is an infrastructure failure, not a skip.

The smoke instantiates the module, wires `Module.submit` to the dropped-in
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

## In-browser ExtendDB edition (P6f-d, pre-release)

The submit edition (`ddbsh-wasm-extenddb-wasm`, output
`ddbsh-extenddb.js`/`ddbsh-extenddb.wasm`)
runs an **in-browser ExtendDB** engine (in-memory SQLite) with **no server, no
network, no credentials, no TLS** — the whole shell runs in-process in the tab.
Because it holds no credentials and makes no AWS call, this edition is safe to
host on any static server (including a public one).

It depends on one third-party runtime artifact: the **ExtendDB WebAssembly
adapter** (`extenddb-wasm-adapter`). **ddbsh does not ship or commit that
adapter** (D-P6f-8); it is a **drop-in**. `web/vendor/extenddb/` holds only the
`PROVENANCE.md` (the pinned version, source, and contract) and a `.gitignore`;
the adapter files themselves are git-ignored and dropped in at build/serve time:

```bash
# Drop the pinned adapter into a bundle's vendor/extenddb/ (dev/test loop):
devtools/stage-extenddb-webasm build-preset/wasm-release/web
# Source: DDBSH_EXTENDDB_WEBASM_PKG
#   (default ~/source/extenddb-webasm/samples/webasm-storefront/pkg).
```

`devtools/run-web-smoke` stages it into the build output automatically before the
Node smoke, and `devtools/serve-web` stages it into the served webroot before
serving, so the usual dev loop needs no manual step. A **deploy** drops an
`extenddb-wasm-adapter@0.1.26` build into the served webroot's `vendor/extenddb/`
by hand (same-origin with `index.html` is mandatory). Because the adapter lives
in the webroot independent of ddbsh's build, a new adapter release is a
**swap-the-dir-and-relaunch** — no ddbsh rebuild — as long as its
`create()`/`submit()` contract and filenames hold.

**Pre-release status:** the adapter self-describes as an internal crate "with no
API stability promise", so it is pinned exactly to `0.1.26`. This edition is
marked pre-release until an official, stable ExtendDB WebAssembly release exists;
setups 1 and 2 (the real-DynamoDB and real-ExtendDB-server proxy paths) are the
production paths.

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
| CONNECT | (client reconfigure) | **No-op** in-browser (single fixed backend); refreshes the region label only. In the **fetch edition** `CONNECT` forwards to `ddbsh-web-proxy` (`/connect`) so the proxy re-targets the session's region/endpoint. |

## Boundaries (unchanged invariants)

- The engine (`include/ddbsh/*.hpp`, `src/`) and `ddbsh-wire` stay SDK-free and
  terminal-free (`engine_boundary_check`). `<emscripten.h>` appears only in
  `transport_browser.cpp` and `transport_fetch.cpp` (the WASM target, not
  `ddbsh-wire`) and in the single pre-ratified `#if __EMSCRIPTEN__` arm of
  `WireClient::poll_delay`.
- The batch-execution policy is shared with the CLI via
  `ddbsh::cli::run_batch` (`cli/repl.*`) — no duplicated, drift-prone copy.
- The CLI and the real-DDB golden suite are untouched; the golden contract
  stays green.
