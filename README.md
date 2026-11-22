# ddbsh

ddbsh is a simple CLI for DynamoDB modeled on isql, and the MySQL CLIs. ddbsh is open source. For more information about this, see the [Licensing](#licensing) section below.

ddbsh presents the user with a simple command line interface. Here the user can enter SQL-like commands to DynamoDB. The output is presented in the same window. ddbsh supports many Data Definition Language ([DDL](docs/reference/ddl.md)) and Data Manipulation Language ([DML](docs/reference/dml.md)) commands.

It is also an embeddable **C++23 library** with an SDK-free, terminal-free engine
(lexer, parser, command model, expression engine, SQL→API planner, execution,
and output/EXPLAIN formatting), a thin `ddbsh` command that wraps it, and a
browser/WebAssembly build with a web UI. Current version: **0.12.1**. The engine
drives the full standard `ddbsh` command surface, including global tables, end
to end against real DynamoDB.

---
**WARNING**
ddbsh is provided for your use on an AS-IS basis, is not supported for production use cases, and should only be used for non-production and experimental use cases. Please refer to [section 7 of the License](LICENSE) for more details.

ddbsh can operate directly against your dynamodb tables and thus delete/drop will impact your tables and the operations are irreversible. ddbsh can perform scans and queries against your data and the operations you perform count against your tables capacity, and could incur significant costs.

For details on how to restrict access to specific APIs, refer to the documentation on [Fine Grained Access Control in DynamoDB](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/authentication-and-access-control.html).

It is strongly advised that you understand what ddbsh is doing, and experiment with [ExtendDB](docs/configurations/02-native-extenddb.md) (a local, self-hosted DynamoDB-compatible server) first.

---

## Quickstart

Build the library and shell (full details in [docs/BUILDING.md](docs/BUILDING.md)):

```sh
cmake -S . -B build
cmake --build build
```

Run one command against DynamoDB (credentials and region resolve through the
standard AWS chain):

```sh
./build/ddbsh -c 'select * from Movies where year = 2014;'
```

Or start the interactive shell and type SQL-like statements terminated by `;`:

```
$ ddbsh
ddbsh - version 0.12.1
us-east-1> show tables;
```

## The three configurations at a glance

ddbsh runs in three configurations, formed by two choices: **which form of
ddbsh** (the native binary or the browser edition) and **which backend** (real
Amazon DynamoDB or a self-hosted [ExtendDB](https://github.com/awslabs/extenddb)
server).

| # | ddbsh form | Backend | Page |
|---|---|---|---|
| 1 | native binary | AWS DynamoDB | [docs/configurations/01-native-aws.md](docs/configurations/01-native-aws.md) |
| 2 | native binary | ExtendDB on a host | [docs/configurations/02-native-extenddb.md](docs/configurations/02-native-extenddb.md) |
| 3 | browser edition | AWS DynamoDB | [docs/configurations/03-browser-aws.md](docs/configurations/03-browser-aws.md) |

See the [configurations index](docs/configurations/README.md) for getting
started, environment variables, and cross-account access.

## Browser edition

The browser edition is a static web page: the same engine compiled to
WebAssembly, plus a pinned copy of the AWS SDK for JavaScript that signs each
request and sends it from the tab straight to DynamoDB. There is no server-side
component. You paste short-term credentials (`aws sts get-session-token`,
`aws sts assume-role`, or SSO `export-credentials`); they stay in the tab's
memory. Long-term `AKIA…` keys and non-AWS endpoints are refused. Build and
serve it locally with:

```sh
emcmake cmake --preset wasm-release && cmake --build build-preset/wasm-release
devtools/serve-web 8080        # loopback only; open http://localhost:8080/
```

Details: [docs/configurations/03-browser-aws.md](docs/configurations/03-browser-aws.md).
To publish the page for others (a private S3 bucket behind CloudFront, or
another host), see [docs/HOSTING.md](docs/HOSTING.md).

## Documentation

- **[Building](docs/BUILDING.md)** — toolchain, CMake, platforms, installing a
  release, and building the AWS SDK for C++ from source.
- **[Testing](docs/TESTING.md)** — `devtools/run-tests`, the golden suite, and
  the native unit tests.
- **[Contributing](docs/CONTRIBUTING.md)** — how to contribute and the project
  direction.
- **[Embedding the library](docs/EMBEDDING.md)** — the `Connection`/`Database`
  API, `ddbsh::aws::open`, the offline `MemoryClient`, and `find_package(ddbsh)`.
- **[Running ddbsh: configurations](docs/configurations/README.md)** — the three
  run configurations, one page each.
- **[Hosting the browser edition](docs/HOSTING.md)** — publish the page on its
  own origin: the CloudFormation template, upload, checks, updates, teardown.
- **[Command reference](docs/reference/README.md)** — the full SQL-like command
  surface: [control](docs/reference/control.md),
  [DDL](docs/reference/ddl.md), [DML](docs/reference/dml.md), and
  [special topics](docs/reference/special-topics.md).

Developer notes for the browser edition (layout, rebuilding the bundled AWS SDK
for JavaScript, the web gates) are in [`web/README.md`](web/README.md).

## Direction

Planned work (bugs, new language constructs, signed binaries, and more) is
tracked in [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md#direction).

## Licensing

ddbsh is licensed under the terms of the Apache Version 2.0 (January 2004) license. See the License file [here](LICENSE).
