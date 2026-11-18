<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Configuration 3 — browser (WebAssembly) against AWS DynamoDB

The SDK-free ddbsh engine compiles to WebAssembly and runs in a browser. A
browser cannot read `~/.aws/credentials` and cannot call DynamoDB directly (no
CORS), so this configuration uses the native **`ddbsh-web-proxy`**, which signs
requests with SigV4 using the host's AWS credentials and forwards them to
DynamoDB. Credentials stay on the host and never enter the browser.

**Production-ready.**

See [`web/README.md`](../../web/README.md) and
[`proxy/README.md`](../../proxy/README.md) for the build steps, the proxy's
security model (loopback-only ingress, origin + per-session-token guard, a
user-owned egress allowlist), and the per-endpoint credential/CA configuration.

For the full five-configuration overview, see the
[configurations index](README.md).
