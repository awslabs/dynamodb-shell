<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# ddbsh library embedding examples

Two worked examples of the same library/CLI boundary (tenet 3), differing only
in the `IDDBClient` they use:

- **`embed_demo`** — OFFLINE. Supplies its own in-memory `IDDBClient`, so it
  needs no AWS account and incurs no cost. Builds with a plain Makefile against
  the SDK-free engine (`-lddbsh`). Deterministic; the project's `embed_sample`
  CTest diffs its output against a committed golden.
- **`live_demo`** (in `live_demo/`) — LIVE. Uses the ready-made
  `ddbsh::aws::AwsDDBClient` (the real AWS SDK DynamoDB client) against **real
  DynamoDB**. Builds as an external `find_package(ddbsh)` CMake consumer linking
  `ddbsh::aws`. It creates and deletes a real table (self-cleaning) and costs
  real (tiny) money.

## `embed_demo` (offline)

`embed_demo` is a standalone program that embeds the ddbsh engine library
(`libddbsh`). It exists to prove the library is consumable on its own — it
builds with a **plain Makefile** (not the project's CMake) against the
**installed public headers and library**, and links only `-lddbsh`.

It demonstrates the full embedding path end to end, offline:

1. Implement the injected client seam `ddbsh::IDDBClient` (here a tiny
   in-memory fake, so the demo needs no AWS account and incurs no cost).
2. Construct a `ddbsh::Session` from a `ddbsh::SessionConfig` and the borrowed
   client — no globals, no singleton.
3. Parse and execute SQL-ish statements via `Session::run()`.
4. Render the structured `ExecOutcome` (or `Error`) to the exact ddbsh output
   bytes with `ddbsh::render::Renderer`.

The engine does the SQL→API planning (GetItem vs Query vs Scan) and all output
formatting; the sample's `IDDBClient` supplies the "service" responses. The
in-memory client also implements the write ops, so the demo exercises a
**mutating command path** end to end — `INSERT`, `UPDATE ... WHERE <full key>`,
and `DELETE ... WHERE <full key>` — alongside the read commands, all offline.

## Build and run

First build and install the library to a prefix (from the repo root):

```sh
cmake -S . -B build
cmake --build build
cmake --install build --prefix /path/to/prefix
```

Then build and run the sample against that prefix:

```sh
cd examples
make DDBSH_PREFIX=/path/to/prefix
make run DDBSH_PREFIX=/path/to/prefix
```

`DDBSH_PREFIX` defaults to `/usr/local`. `make run` sets an rpath to the
install `lib` directory, so no `LD_LIBRARY_PATH` is needed.

## `live_demo` (real DynamoDB, `ddbsh::aws`)

`live_demo/live_demo.cpp` is the LIVE counterpart. Instead of writing an
`IDDBClient`, it uses the ready-made **`ddbsh::aws::AwsDDBClient`** — the AWS
SDK-backed adapter the CLI and the golden suite run on, shipped as the optional
`ddbsh::aws` component — plus `ddbsh::aws::AwsApiGuard` for SDK lifecycle. It
runs a real sequence against real DynamoDB:

`CREATE TABLE` (on-demand) → `INSERT` → `SELECT` three ways (full key → GetItem,
hash key → Query, no WHERE → Scan) → `DROP TABLE`.

The table name is uniquified per run and a teardown scope-guard drops it on
every exit path, so a run never strands a live table. It shows **both**
result-consumption paths:

1. the application path — iterate typed rows with `ddbsh::items()` and print
   each cell with `ddbsh::render::render_value()`, no variant handling; and
2. the byte-exact path — hand the whole `ExecOutcome` to
   `ddbsh::render::Renderer`.

It builds as an external CMake consumer of the installed `find_package(ddbsh)`
package (it needs the AWS SDK's transitive dependencies resolved, which a plain
Makefile cannot do):

```sh
# install the library + the ddbsh::aws component first
cmake -S . -B build
cmake --build build
cmake --install build --prefix /path/to/prefix

# then build the live demo against that install (+ the AWS SDK)
cmake -S examples/live_demo -B build-live \
    -DCMAKE_PREFIX_PATH="/path/to/prefix;/path/to/aws-sdk-cpp"
cmake --build build-live
./build-live/live_demo        # talks to REAL DynamoDB — costs real money
```

> **WARNING:** `live_demo` creates and deletes a real DynamoDB table and
> writes/reads real items, incurring real (tiny) cost. Credentials/region
> resolve through the standard AWS chain (`~/.aws`, `AWS_PROFILE`, `AWS_REGION`;
> default region `us-east-1`).

As a CTest, it is `live_demo_sample` — OFF by default (it spends and needs
credentials); enable with `-DDDBSH_TEST_LIVE_SAMPLE=ON`. The harness
(`run_live_test.sh`) installs to a throwaway prefix, builds the demo as a
`find_package(ddbsh)` consumer, runs it against real DynamoDB, echoes the target
(endpoint + access key id), and asserts the run completes.

## Writing your own `IDDBClient`

The offline `embed_demo` implements `IDDBClient` by hand against an in-memory
table. A real embedder who does not want the ready-made `ddbsh::aws` client can
likewise implement `IDDBClient` against the AWS SDK for C++ — translating the
library's SDK-free request/response structs to and from
`Aws::DynamoDB::Model::*` — exactly as `aws/aws_ddb_client.cpp` does. That
translation lives in the `ddbsh::aws` component (not the engine) so the engine
library stays SDK-free and terminal-free.
