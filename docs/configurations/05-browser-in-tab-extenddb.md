<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Configuration 5 — ExtendDB bundled in the browser (pre-release)

An in-tab [ExtendDB](https://github.com/awslabs/extenddb) WebAssembly database,
reached over an in-process transport with no network, no credentials, and no
TLS. The ddbsh engine and ExtendDB run side by side in the same browser tab.

**Pre-release.** ddbsh does not ship the ExtendDB WebAssembly adapter blob; it is
a drop-in placed under `web/vendor/extenddb/`, and this configuration is finished
when an official ExtendDB WebAssembly build is released.

See [`web/README.md`](../../web/README.md) for the build steps and the
single-webroot bundling model (drop in the ExtendDB WebAssembly build, then
swap-and-relaunch without rebuilding ddbsh).

For the full five-configuration overview, see the
[configurations index](README.md).
