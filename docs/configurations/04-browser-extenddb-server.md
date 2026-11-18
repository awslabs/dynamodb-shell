<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Configuration 4 — browser (WebAssembly) against a local ExtendDB server

The browser/WebAssembly ddbsh build talking to a self-hosted
[ExtendDB](https://github.com/awslabs/extenddb) server through the native
**`ddbsh-web-proxy`**. It is the same proxy used in
[configuration 3](03-browser-aws.md), configured with a per-endpoint profile
(the ExtendDB-issued SigV4 key) and a per-endpoint CA bundle for ExtendDB's
self-signed certificate. Credentials stay on the host and never enter the
browser.

**Production-ready.**

ExtendDB itself is set up exactly as in
[configuration 2](02-native-extenddb.md) (build, start, issue a key, trust the
self-signed cert). See [`web/README.md`](../../web/README.md) and
[`proxy/README.md`](../../proxy/README.md) for the browser build steps, the
proxy's security model (loopback-only ingress, origin + per-session-token guard,
a user-owned egress allowlist), and the per-endpoint credential/CA
configuration.

For the full five-configuration overview, see the
[configurations index](README.md).
