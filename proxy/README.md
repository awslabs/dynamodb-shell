# ddbsh-web-proxy

The native local companion for the ddbsh real-backend web UI (phase 6f,
D-P6f-3). A small, **generic** SigV4 signing relay — it contains no ddbsh engine
logic. The engine ships as the `ddbsh-wasm` WebAssembly artifact (see `../web/`);
this binary exists only because a browser cannot read `~/.aws/credentials` and
cannot call DynamoDB directly (DynamoDB returns no CORS headers).

## What it does

1. **Serves** the static ddbsh-wasm bundle (`--serve --webroot <dir>`).
2. Holds **per-session connection state** keyed by the `X-Ddbsh-Session` header a
   browser tab mints at load (a tab ≈ a ddbsh process).
3. On `POST /ddb`, **SigV4-signs** the browser's AWS-JSON-1.0 body with
   credentials resolved from the host's AWS chain and **forwards** it to the
   session's DynamoDB / ExtendDB endpoint, relaying the response (including a
   DynamoDB `__type` error document) back for the `ddbsh-wire` codec to map.

Credentials never reach the browser. We accept no key, store no key, and mint no
key: AWS credentials come from the standard provider chain (env, `~/.aws`, SSO,
assume-role, IMDS); a non-AWS backend (ExtendDB) uses its own issued key in a
dedicated `~/.aws` profile selected with the `profile` field on the `/connect`
request. ExtendDB is **HTTPS-only** with a self-signed cert, so `/connect` also
carries a `ca_bundle` field (the cert's path) and the proxy builds a
**per-endpoint-trust** outbound client for it; plain HTTP is never used for
ExtendDB. Both `profile` and `ca_bundle` are non-secret (a profile name and a
file path) — the key itself stays in the credentials file the proxy reads.

## Endpoints

| Method / path | Purpose | Auth |
|---|---|---|
| `POST /session` | Mint `{session, token}` for this page | origin + host guard |
| `POST /connect` | Set the session's target `{region, endpoint?, profile?, ca_bundle?}` | guard + session token; egress-policy checked |
| `POST /ddb` | Sign + forward (`X-Ddbsh-Op` = the DynamoDB operation) | guard + session token |
| `GET /healthz` | Loopback liveness probe (no secrets) | — |
| `GET /*` | Static file from `--webroot` (when `--serve`) | — |

## Security model (builder-security, followed as policy)

Three fixed layers plus one user-owned file:

1. **Ingress — loopback only, not configurable.** The proxy always binds
   `127.0.0.1` (and `::1` when available). There is deliberately **no** option to
   bind a non-loopback address, so it can never become an open credentialed
   relay. Remote access is the user's SSH local-forward tunnel
   (`ssh -L <port>:127.0.0.1:<port> host`), whose auth and encryption are the
   gate. `--port` chooses the loopback port.
2. **Driver guard — origin + per-session token (mandatory).** Loopback stops
   other *machines*; this stops a malicious page open in the *same browser* from
   driving the proxy via `fetch('http://localhost:PORT/…')` (confused-deputy /
   DNS-rebinding). Every `/session`, `/connect`, `/ddb` request must carry an
   `Origin` **and** `Host` that name the served page's own loopback origin, and
   `/connect` + `/ddb` additionally require the per-session token minted at
   `/session` (compared in constant time). The proxy sends no CORS headers, so a
   cross-origin page cannot read a response even if it reached one.
3. **Egress policy — user-owned, startup-only.** `~/.ddbsh_web_proxy_security`
   (override with `--security-policy <file>`). Anchored allow/block host regexes;
   an endpoint is permitted iff its canonical host matches ≥1 `allow` and 0
   `block` rules (**block wins**). The default (used when the file is absent)
   allows loopback and the DynamoDB endpoint host families across the aws /
   aws-us-gov / aws-cn partitions (FIPS + dual-stack), and blocks the IMDS
   address. Read **once at startup** — edit the file and restart to apply. A
   disallowed `CONNECT` endpoint is rejected before any credential is resolved.

   To reach a remote ExtendDB or a gamma/internal endpoint, add a one-line
   `allow` entry and restart. Example `~/.ddbsh_web_proxy_security`:

   ```
   # keep the defaults by re-stating the ones you need, then add yours:
   allow localhost
   allow localhost:\d{1,5}
   allow dynamodb\.[a-z0-9-]+\.amazonaws\.com
   allow extenddb\.corp\.example\.com:18443
   block 169\.254\.169\.254
   ```

   (The file fully replaces the built-in default when present, so re-state the
   default `allow` lines you still want.)

## Build & run

```bash
cd proxy
cargo build --release
./target/release/ddbsh-web-proxy --serve --webroot ../build-preset/wasm-release/web
# open http://127.0.0.1:8456/  (the default port; override with --port)
```

Options: `--port <N>` (default 8456), `--serve`, `--webroot <DIR>`,
`--security-policy <F>`, `--session-ttl <SECS>` (default 1800),
`--max-sessions <N>` (default 256), `-h/--help`.

## Test

```bash
cargo test            # pure-logic unit tests (egress policy, guard, sessions, op validation)
cargo clippy --all-targets -- -D warnings
cargo fmt --check
```

The signing + forwarding path is covered end-to-end against **real DynamoDB** by
`../devtools/run-proxy-smoke` and against a **real local ExtendDB** server
(HTTPS, self-signed cert, per-endpoint `profile` + `ca_bundle`) by
`../devtools/run-extenddb-smoke` — both drive the `ddbsh-wasm` edition through a
running proxy.

## Portability

A single static binary; a trivial `--target` cross-compile. Built and tested on
Linux here; Windows and macOS are made to build (portable code; a CI matrix
proves the build — D-P6f-11).
