<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# ddbsh library embedding examples

Two worked examples of the **embedding API** — the simple, ODBC / Sybase
DB-Library-style surface (`ddbsh::Connection` / `Results` / `Row` / `Cell`).
They differ only in the backend they open over:

- **`ddbshlib_inmemory_sample`** (in `ddbshlib_inmemory_sample/`) — OFFLINE.
  Drives the API over the in-memory `ddbsh::testing::MemoryClient`, so it needs
  no AWS account and incurs no cost. Links only the SDK-free engine
  (`ddbsh::ddbsh`). Built as a default-on, credential-less CTest.
- **`ddbshlib_sample`** (in `ddbshlib_sample/`) — LIVE. Opens a `Database` with
  `ddbsh::aws::open()` against **real DynamoDB**. Links `ddbsh::aws` (which
  pulls `ddbsh::ddbsh` + the AWS SDK). It creates and deletes a real table
  (self-cleaning) and costs real (tiny) money.

Both are standalone external consumers of the installed `find_package(ddbsh)`
package, each with its own `CMakeLists.txt`.

## `ddbshlib_inmemory_sample` (offline)

Drives the embedding API with **no AWS account, no credentials, no network,
zero cost**:

1. Construct a `ddbsh::testing::MemoryClient` (a genuine in-memory store).
2. Open a `ddbsh::Connection` over it (`SessionConfig` + the borrowed client).
3. `exec()` CREATE / INSERT, then `query()` typed rows and `render_all()` the
   byte-exact output.

It exercises all three access plans (full key → GetItem, hash key → Query,
no WHERE → Scan) end to end against stored data. `MemoryClient` is SDK-free and
is the recommended way to unit-test your own ddbsh usage offline.

```sh
# install the engine first (DDBSH_BUILD_AWS may be OFF — no SDK needed)
cmake -S . -B build
cmake --build build
cmake --install build --prefix /path/to/prefix

# then build the offline sample against that install
cmake -S examples/ddbshlib_inmemory_sample -B build-inmem \
    -DCMAKE_PREFIX_PATH="/path/to/prefix"
cmake --build build-inmem
./build-inmem/ddbshlib_inmemory_sample
```

As a CTest it is `ddbshlib_inmemory_sample`, built in-tree and run by default
(no credentials, no cost).

## `ddbshlib_sample` (real DynamoDB, `ddbsh::aws::open`)

The LIVE counterpart. It opens a `Database` with one call —
`ddbsh::aws::open(region)` resolves region/credentials and manages the AWS SDK
lifecycle internally (no `AwsApiGuard`) — then runs:

`CREATE TABLE` (on-demand) → `INSERT` → `SELECT` three ways (full key → GetItem,
hash key → Query, no WHERE → Scan) → `DROP TABLE`.

The table name is uniquified per run and an RAII scope-guard drops it on every
exit path, so a run never strands a live table. Every `Result` is checked; the
hash-key query is consumed as typed `Row`s, the others via the byte-exact
`render_all()` path.

```sh
# install the library + the ddbsh::aws component first
cmake -S . -B build
cmake --build build
cmake --install build --prefix /path/to/prefix

# then build the live sample against that install (+ the AWS SDK)
cmake -S examples/ddbshlib_sample -B build-sample \
    -DCMAKE_PREFIX_PATH="/path/to/prefix;/path/to/aws-sdk-cpp"
cmake --build build-sample
./build-sample/ddbshlib_sample        # talks to REAL DynamoDB — costs money
```

> **WARNING:** `ddbshlib_sample` creates and deletes a real DynamoDB table and
> writes/reads real items, incurring real (tiny) cost. Credentials/region
> resolve through the standard AWS chain (`~/.aws`, `AWS_PROFILE`, `AWS_REGION`;
> default region `us-east-1`).

As CTests, the live sample (`ddbshlib_sample`) and the real-DynamoDB functional
test (`ddbsh_live_api`, a GoogleTest binary exercising the embedding API against
a real table) are OFF by default; enable them with `-DDDBSH_TEST_LIVE_SAMPLE=ON`
where credentials are present.

## Choosing a backend

`ddbsh::aws::open()` is the simplest way to go live. If you need a custom
backend (a different SDK, a mock, a proxy), implement `ddbsh::IDDBClient`
yourself and open a `ddbsh::Connection` over it — exactly as `MemoryClient`
does for the offline sample and `ddbsh::aws::AwsDDBClient`
(`aws/aws_ddb_client.cpp`) does for the AWS SDK. That translation lives in the
`ddbsh::aws` component (not the engine), so the engine library stays SDK-free
and terminal-free.
