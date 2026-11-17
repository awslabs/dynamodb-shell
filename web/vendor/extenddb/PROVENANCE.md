# Vendored third-party artifact — ExtendDB WASM adapter

This directory holds a **pinned, vendored** copy of the in-browser ExtendDB
WebAssembly adapter. The ddbsh build never builds ExtendDB from source; it links
nothing from it at C++ link time. The adapter is a runtime JS/WASM dependency of
the browser web target only: the page wires `Module.submit(op, body)` to the
adapter's `submit`, and the engine's `SubmitTransport` bridges to it (P6d).

| Field | Value |
|-------|-------|
| Package | `extenddb-wasm-adapter` |
| Version | **0.1.26** (pinned exactly; never floated) |
| License | Apache-2.0 (see `LICENSE`) |
| Source  | `~/source/extenddb-webasm/samples/webasm-storefront/pkg` (read-only) |
| Upstream | https://github.com/ExtendDB/extenddb — https://extenddb.org/ |
| Vendored | 2026-11-17 (P6d, `dev/p6d-wasm-target`) |

## Version note — why 0.1.26, not the architect's 0.1.21 (D-P6d-6 deviation)

The P6d design (D-P6d-6) pinned `0.1.21`, read from
`crates/wasm-adapter/pkg` — a **compile-only** verification of the
`create()`/`submit()` contract. Running that `0.1.21` artifact under the P6d
Node smoke revealed a genuine **runtime** defect: the engine panics on the
first table-creating op with `std::time ... "time not implemented on this
platform"` (a wasm32 `SystemTime`/`Instant` bug). The upstream repo's own
working headless demo (`samples/webasm-storefront/headless/run.mjs`) runs
against the newer **`0.1.26`** build in `samples/webasm-storefront/pkg`, whose
runner comment records that this exact `std::time` bug was fixed. The `create()`
/ `submit()` TypeScript contract is **byte-identical** between 0.1.21 and 0.1.26
(verified with `diff` on the `.d.ts`), so the P6d transport/bridge design is
unchanged — only the artifact that actually runs is newer. Vendoring 0.1.26
(the version the upstream demo ships and this project's smoke passes against) is
a deliberate, correctness-driven deviation from D-P6d-6, recorded for the
reviewer/principal. The pin is still exact and never floated.

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

## Stability note

The package self-describes as an internal crate "with no API stability
promise." It is therefore pinned to the exact `0.1.26` artifact set; bumping it
is a deliberate, reviewed change (re-verify the `create()` / `submit()` contract
and re-run the P6d Node smoke).

## Files

| File | Role |
|------|------|
| `extenddb_wasm_adapter.js` | ES-module glue (default export `__wbg_init`, `initSync`, `ExtendDb`) |
| `extenddb_wasm_adapter_bg.wasm` | the ExtendDB engine (in-memory SQLite) |
| `extenddb_wasm_adapter.d.ts` | TypeScript surface |
| `extenddb_wasm_adapter_bg.wasm.d.ts` | low-level wasm export types |
| `package.json` | upstream manifest (records name/version/license) |
| `LICENSE` | upstream Apache-2.0 license text |
