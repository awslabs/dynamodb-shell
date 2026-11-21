<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Testing

Run the test suite via `devtools/run-tests` from the orchestration repo. Since
the Step-2 exit gate, every non-global-tables golden family runs against the
new engine (the shipping `ddbsh` binary driven through the external harness in
`tests/golden/`); the gate exit propagates. The original C++11 engine was
removed from the tree during the phase-2i oracle retirement (preserved in git
history under the `oracle-archive-v0.8.1` tag); its golden contract was
relocated to a single command-family tree at `tests/golden/` and remains the
regression source. The
native GoogleTest unit tests (offline, `FakeDDBClient`)
build with the project (`ddbsh-tests`) and run alongside as the deterministic
backend-free fidelity signal.

## Running the native unit tests directly

Once you complete a build you can run the native unit tests directly with
CTest:

```
% ctest --test-dir .
Running tests...
Test project /Users/amrithie/source/github/dynamodb-shell/build
        Start   1: ddbsh-basic-cli
[...]
```


## Browser edition gates

The browser edition (ddbsh.wasm + the bundled AWS SDK for JavaScript) has its own
gates. Both need emsdk and Node 20+ (see [BUILDING](BUILDING.md#build-the-browser-edition));
the live parts talk to real DynamoDB.

- **`devtools/run-tests --web-golden`** (or `devtools/run-web-golden`) runs the
  **whole golden suite** through the browser code path: the golden runner drives
  `web/ddbsh-node.mjs`, a CLI-shaped Node front end that loads the same files as
  the page, against real DynamoDB, and diffs against the same `.exp` files as
  the native CLI. The only expectation that differs is the `ishow-connection`
  `Backend:` pattern (`DDBSH_GOLDEN_BACKEND_REGEX`). Artifacts:
  `discussions/test-webgolden-<hash>.{log,txt}`.
- **`devtools/run-tests --web-smoke`** (or `devtools/run-web-smoke`) builds the bundle, runs the offline gates (the
  glue has no `eval`/`new Function`; `app-core.test.mjs`; `sdk-bridge.test.mjs`,
  which exercises the real SDK bundle with a stub HTTP handler; a grep that keeps
  browser storage away from credentials), then a short live smoke through the
  page's `DdbshSession.run` path on one `ddbsh_web_smoke_<pid>` table. Artifact:
  `discussions/test-websmoke-<hash>.log`.

Run both through the orchestration repo's `devtools/run-tests` (same flags), which
copies the artifacts into its `discussions/`.

Both scripts mint short-term credentials with `aws sts get-session-token` (for
the `default` profile, plus `cross-acct` for the golden suite; lifetime
`DDBSH_WEB_CRED_SECONDS`, default 7200) into a private temporary directory that
is removed on exit, and echo the minted access key id and the caller ARN. A
minting failure fails the run; nothing is skipped.

The native and web golden suites create and drop the same `ddbsh_test_*` tables,
so they must not run at the same time. Both take a shared lock
(`build-golden.lock`, via `flock` where available) and refuse to start while the
other runs.
