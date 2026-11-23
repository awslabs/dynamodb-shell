<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Testing

`devtools/run-tests` runs the test suite (`--help` lists its options). It has
two parts, and a failure in either fails the run:

- **The golden suite** drives the shipping `ddbsh` binary through the harness
  in `tests/golden/` against real DynamoDB, and compares its output with the
  `.exp` files there. Every command family runs, global tables included. The
  `.exp` files are the fidelity contract inherited from the original C++11
  ddbsh, whose engine is in git history under the `v0.8.1` tag.
- **The native unit tests** (GoogleTest, offline, with `FakeDDBClient` and
  `MemoryClient`) build with the project (`ddbsh-tests`) and are the
  deterministic, backend-free signal.

The golden suite creates and drops real tables, which costs money: run it in a
dedicated test account. Credentials and the region come from the standard AWS
chain (`~/.aws`, `AWS_PROFILE`, `AWS_REGION`; the default region is
`us-east-1`), and the cross-account family uses the `cross-acct` profile. Before
any test runs, the script echoes the target (endpoint, access key id and caller
ARN). With `DDBSH_REDACT_KEY_ID=1`, which CI sets, the echo shows the access key
id only by its first and last four characters. The script's artifacts are
`discussions/test-<suite>-<hash>.{log,txt}`, named by the commit.

## Running the native unit tests directly

After a build, run the native unit tests with CTest from the build directory:

```sh
ctest --test-dir build --output-on-failure
```

With the CMake presets, `ctest --preset linux-debug`, or
`ctest --preset linux-sanitize` for a build with ASan, UBSan and LSan.

## Continuous integration

GitHub Actions runs the suites in AWS accounts of their own. [CI](CI.md) has
the details for maintainers.

- **On every pull request, every push to `main`, and on demand**
  (`.github/workflows/test.yml`): the native unit tests; the same tests under
  ASan, UBSan and LSan; a clang-format 15 check of every C/C++ file; and the
  golden suite against real DynamoDB.
- **On every pull request, a created release, and on demand**
  (`.github/workflows/build.yml`): the release binary, and the browser bundle
  with the offline web gates.
- **Where the golden suite runs.** In two dedicated AWS accounts, which GitHub
  signs in to through OIDC with no stored credentials: one for the whole suite
  (us-east-1, plus us-east-2 and us-west-1 for the global-tables family) and one
  for the cross-account family. Local runs never use these accounts.
- **Pull requests from forks.** GitHub gives them no OIDC token, so the golden
  suite cannot run. Instead, the job "Golden suite (real DynamoDB): fork pull
  request" fails with an explanation. A maintainer can run the suite after
  reviewing the whole change: pushing it to `awslabs/dynamodb-shell` runs it
  with the repository's secrets and `GITHUB_TOKEN`, and with the CI role
  ([CI](CI.md#pull-requests-from-forks)).
- **One golden run at a time.** The suite's table names are fixed, so the
  golden job queues. A running job is never cancelled; a newer waiting job
  replaces an older waiting one.
- **Not in CI:** the live web gates below, which need credentials in the
  browser code path.

## Browser edition gates

The browser edition (ddbsh.wasm + the bundled AWS SDK for JavaScript) has its own
gates. Both need emsdk and Node 20+ (see [BUILDING](BUILDING.md#build-the-browser-edition));
the live parts talk to real DynamoDB.

- **`devtools/run-tests --web-golden`** (or `devtools/run-web-golden`) runs the
  offline gates, then the **whole golden suite** through the browser code path:
  the golden runner drives
  `web/ddbsh-node.mjs`, a CLI-shaped Node front end that loads the same files as
  the page, against real DynamoDB, and diffs against the same `.exp` files as
  the native CLI. The only expectation that differs is the `ishow-connection`
  `Backend:` pattern (`DDBSH_GOLDEN_BACKEND_REGEX`). Artifacts:
  `discussions/test-webgolden-<hash>.{log,txt}`.
- **`devtools/run-tests --web-smoke`** (or `devtools/run-web-smoke`) builds and
  packages the bundle and runs the offline gates, which CI and `--web-golden`
  run too: the glue has no `eval`/`new Function`; `app-core.test.mjs`;
  `sdk-bridge.test.mjs`, which exercises the real SDK bundle with a stub HTTP
  handler; `streaming.test.mjs`, which runs the built engine's streaming entry
  point with stub transport hooks and checks that each row reaches the page
  before the next request; `package.test.mjs`, which checks the packaged folder, zip and
  `SHA256SUMS` against the publish allowlist and that `package.cmake` refuses
  bad input; a grep that keeps browser storage away from credentials; and a
  check that `web/hosting/aws/deploy.sh` and [HOSTING](HOSTING.md) carry the
  same upload commands. Then it runs a short live smoke through the page's
  `DdbshSession.run` and `runStreaming` paths on one `ddbsh_web_smoke_<pid>`
  table. Artifact:
  `discussions/test-websmoke-<hash>.log`.
- **`devtools/check-web-hosting <stack> [label]`** checks a deployed
  [hosting](HOSTING.md) stack from outside: every published file over HTTPS
  (status, content type, `Cache-Control`, the eight security headers, sha256
  against `SHA256SUMS`), the HTTP-to-HTTPS redirect, and that the bucket is
  private. Run it with credentials that can read the stack and the bucket's
  settings. With **`--anonymous <site-url>`** it checks the served files by the
  site's address and needs no credentials; `--bucket-domain <name>` adds the
  anonymous S3 refusals, and `--wait <seconds>` first waits until the site
  serves the packaged bundle.
  Artifact: `discussions/test-<label>-<hash>.log` (label default `s3host`, or
  `anonymous`).

`run-web-golden` and `run-web-smoke` mint short-term credentials with
`aws sts get-session-token` (for
the `default` profile, plus `cross-acct` for the golden suite; lifetime
`DDBSH_WEB_CRED_SECONDS`, default 7200) into a private temporary directory that
is removed on exit, and echo the minted access key id and the caller ARN. A
minting failure fails the run; nothing is skipped.

The native and web golden suites create and drop the same `ddbsh_test_*` tables,
so they must not run at the same time. Both take a shared lock
(`build-golden.lock`, via `flock` where available) and refuse to start while the
other runs.
