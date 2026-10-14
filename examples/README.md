<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# ddbsh library embedding example

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
formatting; the sample's `IDDBClient` supplies the "service" responses.

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

## Going live

This sample is offline by design. A real embedder implements `IDDBClient`
against the AWS SDK for C++ — translating the library's SDK-free request and
response structs to and from `Aws::DynamoDB::Model::*` — exactly as the shipping
CLI does in `cli/aws_ddb_client.cpp`. That translation lives on the CLI side of
the seam so the engine library stays SDK-free and terminal-free.
