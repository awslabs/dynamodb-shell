<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Vendored AWS SDK for JavaScript v3 (browser edition)

`aws-sdk-dynamodb.mjs` is the only SDK file the ddbsh browser page loads. It is
an esbuild bundle of `@aws-sdk/client-dynamodb` exporting `DynamoDBClient` and
the command classes for the 26 operations the ddbsh-wire codec emits
(`entry.mjs`). It is committed so the page needs no CDN and no install step, and
it is reproducible from the committed inputs.

| File | Kind | Notes |
|---|---|---|
| `package.json` | input | exact pins: `@aws-sdk/client-dynamodb`, `esbuild` |
| `package-lock.json` | input | the reproducible install (`npm ci`) |
| `entry.mjs` | input | what the bundle exports |
| `generate.mjs` | input | writes `allowed-hosts.mjs` + `THIRD-PARTY-NOTICES.txt`; rewrites the CSP in `web/index.html` |
| `aws-sdk-dynamodb.mjs` | output | the bundle (third-party code; Apache-2.0 / MIT, see notices) |
| `allowed-hosts.mjs` | output | `SDK_VERSION`, `REGIONS`, `ALLOWED_HOSTS` |
| `THIRD-PARTY-NOTICES.txt` | output | every bundled package, version, license, and license texts |
| `PROVENANCE.json` | output | versions, build command, sha256 of each output, build time |

Commands (Node ≥ 20 and npm registry access needed):

- `devtools/vendor-aws-sdk-js` — rebuild the outputs from the lockfile.
- `devtools/vendor-aws-sdk-js --check` — rebuild in a temp dir; fail on any
  difference from the committed outputs (CI runs this).
- `devtools/vendor-aws-sdk-js --lock` — regenerate the lockfile after changing a
  pin, then rebuild. Commit every changed file here plus `web/index.html`.

`node_modules/` is never committed.
