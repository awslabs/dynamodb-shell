# Third-party drop-in — ExtendDB WASM adapter (NOT committed)

This directory is the **drop-in location** for the in-browser ExtendDB
WebAssembly adapter that the browser *in-tab* edition (`ddbsh-wasm-extenddb-wasm`,
`SubmitTransport`) runs against. Per **D-P6f-8** ddbsh **never ships or commits**
the adapter blob: the ddbsh build stages only ddbsh's own files, and the adapter
is dropped in here (or into the served webroot's `vendor/extenddb/`) by the dev
tooling or the user/deploy. Only this `PROVENANCE.md` and the sibling
`.gitignore` are tracked; the adapter files themselves are git-ignored.

Consequence (the point of the drop-in model): a new extenddb-webasm release is
**swap-the-dir-and-relaunch** — no ddbsh rebuild — as long as the adapter's
`create()` / `submit()` JS contract and filenames hold.

## Pinned version & source

| Field | Value |
|-------|-------|
| Package | `extenddb-wasm-adapter` |
| Version | **0.1.26** (pinned exactly; never floated) |
| License | Apache-2.0 (upstream `LICENSE`; https://github.com/ExtendDB/extenddb) |
| Source  | `~/source/extenddb-webasm/samples/webasm-storefront/pkg` (read-only) |
| Upstream | https://github.com/ExtendDB/extenddb — https://extenddb.org/ |
| Pre-release | the in-browser edition is **pre-release** (README §"Three setups") |

Version note (carried from P6d): the `0.1.26` build in
`samples/webasm-storefront/pkg` is the version the upstream headless demo ships
and this project's smoke passes against. An earlier `0.1.21` (from
`crates/wasm-adapter/pkg`) had a wasm32 `std::time` runtime bug (`"time not
implemented on this platform"`) fixed by `0.1.26`; the `create()`/`submit()`
`.d.ts` contract is byte-identical between the two, so the P6f transport/bridge
design is unaffected by the pin. The package self-describes as an internal crate
"with no API stability promise", so the pin is exact; bumping it is a deliberate,
reviewed change (re-verify the contract and re-run the web smoke).

## How the adapter gets here

- **Dev / test loop (automatic):** `devtools/stage-extenddb-webasm <target-dir>`
  copies the adapter from `DDBSH_EXTENDDB_WEBASM_PKG` (default the Source path
  above) into `<target-dir>/vendor/extenddb/`, validating the pinned version.
  `devtools/run-web-smoke` stages into the build output dir before the Node
  smoke; `devtools/serve-web` stages into the served webroot before serving.
- **Deploy / by hand:** copy the five adapter files below from an
  `extenddb-wasm-adapter@0.1.26` build into the served webroot's
  `vendor/extenddb/` directory (same-origin with `index.html` is mandatory for
  the ES-module import and the glue's relative `_bg.wasm` fetch).

## Contract (from `extenddb_wasm_adapter.d.ts`)

- `static ExtendDb.create(): Promise<ExtendDb>` — opens a fresh in-memory
  SQLite-backed engine (runs catalog migrations; async).
- `ExtendDb.submit(operation: string, body: string): Promise<string>` —
  `operation` is the `X-Amz-Target` suffix (e.g. `"GetItem"`); `body` is the
  AWS-JSON-1.0 request body. Resolves with the response body as a JSON string.
  **Errors are delivered as values, not rejections**: an engine error resolves
  with a DynamoDB-shaped `__type` error document — the same JSON a real DynamoDB
  endpoint returns. The promise rejects only on an adapter fault. This is
  exactly why `ddbsh::wire::Transport` is `(op, body) -> body`: the codec
  inspects the body to decide success vs. error (D-P6d-1).

## Adapter files (dropped in; git-ignored)

| File | Role |
|------|------|
| `extenddb_wasm_adapter.js` | ES-module glue (default export `__wbg_init`, `initSync`, `ExtendDb`) |
| `extenddb_wasm_adapter_bg.wasm` | the ExtendDB engine (in-memory SQLite) |
| `extenddb_wasm_adapter.d.ts` | TypeScript surface |
| `extenddb_wasm_adapter_bg.wasm.d.ts` | low-level wasm export types |
| `package.json` | upstream manifest (records name/version/license) |
