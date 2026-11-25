<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Python

The `ddbsh` Python package runs ddbsh's SQL-like statements against Amazon
DynamoDB from Python. It contains the same C++ engine as the `ddbsh` command
(the parser, the planner that picks GetItem, Query or Scan, and the renderer),
so a statement makes the same DynamoDB requests and prints the same text.
Requests are signed and sent with botocore.

- [Install](#install)
- [Connect](#connect)
- [Text, as the command prints it: `run`](#text-as-the-command-prints-it-run)
- [Rows: the DB-API cursor](#rows-the-db-api-cursor)
- [Types](#types)
- [Errors](#errors)
- [Retries](#retries)
- [Threads](#threads)
- [AWS Lambda](#aws-lambda)
- [Offline: `connect_memory`](#offline-connect_memory)
- [`python -m ddbsh`](#python--m-ddbsh)
- [Building from source](#building-from-source)

## Install

```
pip install ddbsh
```

The package needs Python 3.9 or later. Its one dependency is `boto3`.

## Connect

```python
import ddbsh

conn = ddbsh.connect()
```

`connect()` takes its credentials, region, endpoint and TLS trust from boto3,
so everything that configures boto3 configures ddbsh: `AWS_PROFILE`, the
`~/.aws/config` and `~/.aws/credentials` files, a profile with `role_arn` and
`source_profile` (boto3 performs the AssumeRole), container and instance roles,
and the Lambda execution role. Credentials are read for each request, so
temporary credentials refresh.

| Argument | Default |
|---|---|
| `session` | `boto3.Session()`. Pass your own, for example `boto3.Session(profile_name="test")`. |
| `region` | The session's region (`AWS_DEFAULT_REGION` or the profile's `region`), else `AWS_REGION`, else `us-east-1`. |
| `endpoint_url` | The endpoint boto3 uses for DynamoDB in that region (`AWS_ENDPOINT_URL_DYNAMODB` and a profile's `endpoint_url` included). Set it for a DynamoDB-compatible local server. |
| `verify` | The session's `ca_bundle` (`AWS_CA_BUNDLE` or the profile's `ca_bundle`), else the default trust. `False` turns verification off; a path names a CA bundle. |
| `timeout` | 60 seconds per request. |
| `retry_callback` | None. See [Retries](#retries). |

`CONNECT <region> [WITH ENDPOINT <endpoint>]` in a statement retargets the
connection, as in the command. `conn.region` and `conn.endpoint_overridden`
report the current target, and `SHOW CONNECTION` prints it. Only an endpoint
given as `endpoint_url` or by `CONNECT ... WITH ENDPOINT` counts as an override;
an endpoint boto3 took from `AWS_ENDPOINT_URL_DYNAMODB` or a profile's
`endpoint_url` is shown as `(default)`, and `endpoint_overridden` is False.

```
ddbsh - version 0.12.6
Region: us-east-1
Endpoint: (default)
Backend: wire/botocore 1.35.99 -> https://dynamodb.us-east-1.amazonaws.com (default); credentials ASIA…WXYZ
```

The access key id is shown as its first and last four characters; the secret
and the session token are never shown.

A connection is a context manager; leaving the `with` block closes it.
`close()` also closes the transport's HTTP connections (it calls the
transport's `close()` method when it has one).

## Text, as the command prints it: `run`

`Connection.run(sql)` runs `sql` exactly as `ddbsh -c` does and returns the
text the command prints, standard output and standard error merged in order.
An error is part of the text, as it is on the terminal.

```python
print(conn.run('select * from movies where id = "m1";'))
# {id: m1, title: Inception, yr: 2010}

print(conn.run("select * from movies;", format="json"))
print(conn.run("select * from movies;", format="json", json_flavor="dynamodb"))
print(conn.run("select * from movies;", format="csv"))
print(conn.run("select * from movies;", format="table"))
```

| Argument | Meaning |
|---|---|
| `format` | `"default"`, `"json"`, `"csv"` or `"table"` ([output formats](reference/output-formats.md)). A `set output` line in `sql` changes it, as in the command. |
| `json_flavor` | `"plain"` or `"dynamodb"`. |
| `abort_on_error` | Stop at the first failing statement, like `ddbsh -e`. |
| `on_output` | `on_output(stream, text)` is called for each piece of output as soon as the engine produces it (`stream` is `"stdout"` or `"stderr"`), for example to print a long Scan while it runs. It must not use the connection. If it raises, the run stops: the remaining statements send nothing to DynamoDB, and the exception is raised when `run` returns. (With `connect_memory` the remaining statements still run in memory; their output is dropped.) |

An unknown `format` or `json_flavor` raises `ValueError`.

`run_result(sql, ...)` takes the same arguments and returns
`RunResult(output, segments, status)`: the text, the list of
`(stream, text)` pieces, and the command's exit status (0, or 1 when the last
statement that ran failed).

## Rows: the DB-API cursor

The package follows PEP 249 (DB-API 2.0) with `paramstyle = "qmark"`:

```python
cur = conn.cursor()
cur.execute('select * from movies where id = ? and yr = ?;', ["m1", 2010])
row = cur.fetchone()        # {'id': 'm1', 'yr': Decimal('2010'), 'title': 'Inception'}

cur.execute("select * from movies;")
for row in cur:
    print(row["title"])
```

- **Rows are dicts** of attribute name to value. DynamoDB items have no fixed
  columns, so `cursor.description` lists the attributes of the rows fetched so
  far, in the order first seen, and grows as you fetch. Each entry is a 7-tuple
  `(name, None, None, None, None, None, None)`. It is `None` for a result set
  without rows.
- **A SELECT pages lazily:** rows are read from DynamoDB as you fetch them, so a
  large Scan is never held in memory at once. (A statement with parameters runs
  at `execute` and its rows are held.)
- **Parameters** are `?` markers, bound in order. A marker can stand anywhere a
  value goes, including the operand of `contains(attr, ?)` and inside
  `BEGIN; ... COMMIT;`. Binding is typed, never textual, so it is the safe way
  to put untrusted values into a statement. `executemany` parses the statement
  once and runs it per parameter sequence.
- **Batches.** Without parameters, `execute` takes several `;`-separated
  statements. The cursor starts on the first statement's result set;
  `nextset()` moves to the next one and returns `None` after the last. A
  statement that fails after the first raises from `nextset()`.
- **`rowcount`** is the number of items modified for a bulk UPDATE, UPSERT or
  DELETE, and -1 otherwise.
- **`commit()` and `rollback()` do nothing.** Every statement takes effect when
  it runs. Group writes atomically with `BEGIN; ... COMMIT;` in the SQL.

The cursor also reports what each result set is:

| Attribute | Meaning |
|---|---|
| `kind` | `"rows"`, `"status"`, `"write"`, `"report"`, `"explain"` or `"empty"`. |
| `status` | The status token of a `status` set: `"CREATE"`, `"INSERT"`, `"ALTER"`, `"COMMIT"`, `"ABORT"`, ... |
| `write_result` | `WriteResult(verb, read, modified, ccf)` for a bulk UPDATE/UPSERT/DELETE: items read, items modified, conditional-check failures. |
| `report` | The lines of a DESCRIBE or SHOW report. |
| `explain` | The request text an EXPLAIN prints. |
| `diagnostics` | A transaction's diagnostic lines (data, not an error). |
| `render()` | The result set as the command prints it. |

Each cursor has its own position. All cursors of a connection share its
DynamoDB client, so a `CONNECT` through any of them retargets the connection.

## Types

| DynamoDB | Python, read | Python, bound |
|---|---|---|
| S | `str` | `str` |
| N | `decimal.Decimal` | `int`, `decimal.Decimal` (exact), `float` (as its `repr`) |
| B | `bytes` | `bytes`, `bytearray`, `memoryview` |
| BOOL | `bool` | `bool` |
| NULL | `None` | `None` |
| L | `list` | `list`, `tuple` |
| M | `dict` | `dict` with `str` keys |
| SS / NS / BS | `set` of `str` / `Decimal` / `bytes` | a non-empty `set` or `frozenset` whose elements are all `str`, all numbers or all bytes |

An attribute that is absent is not in the row's dict; an attribute of type NULL
is present with the value `None`. A value that cannot be bound (an empty set, a
mixed set, a non-finite number, a dict with a non-`str` key, another type)
raises `ProgrammingError`.

## Errors

Errors follow the PEP 249 hierarchy, and the classes are also attributes of
`Connection`:

| Class | Raised for |
|---|---|
| `ProgrammingError` | a syntax error, a missing table (`ResourceNotFoundException`), a `ValidationException`, a wrong number of parameters, an unbindable value |
| `IntegrityError` | a failed condition, for example INSERT of an existing key |
| `OperationalError` | throttling that outlasted the retries, a network or credentials failure, a cancelled transaction, any other DynamoDB error |
| `NotSupportedError` | an operation the backend does not model (EXPLAIN on `connect_memory`) |
| `InterfaceError` | a closed connection or cursor |
| `InternalError` | an internal engine error |

`str(exc)` is the exact text the command prints for the error. `exc.code` names
the engine's error code (for example `"parse_error"`), and `exc.service_error`
is DynamoDB's exception name, when there is one. When the transport failed
(no network, no credentials), the original exception is `exc.__cause__`. A
`KeyboardInterrupt` (or `SystemExit`) raised while a request is in flight, or
in a retry or output callback, is raised as itself. In a multi-statement `run`
it also stops the run: the statements after it send nothing to DynamoDB. On a
cursor, a later statement's interrupt is raised by `nextset()`.

## Retries

The engine retries throttling and transient DynamoDB errors itself, with
exponential backoff and jitter, up to 11 attempts; the botocore transport makes
one attempt per request. To see each retry:

```python
def on_retry(event):
    print(f"{event.operation} {event.error_code}: attempt {event.attempt}"
          f" of {event.max_attempts} in {event.delay_ms} ms")

conn = ddbsh.connect(retry_callback=on_retry)
conn.set_retry_callback(None)   # stop
```

`event.throttled` is true for a throttling error. The callback runs on the
thread that made the request, before the backoff sleep. An exception it raises
goes to `sys.unraisablehook` and the retry still happens. The callback must not
use the connection.

## Threads

`threadsafety` is 1: threads may share the module, but each thread should have
its own connection. The engine releases the GIL while it works, so threads with
their own connections run in parallel. A connection that two threads do share
serializes their calls. A transport, retry or output callback that calls back
into its own connection gets a `ProgrammingError` instead of a deadlock.

## AWS Lambda

The package is a regular wheel, so it goes in a Lambda deployment package or
layer like any other dependency (build it for the function's architecture and
Python version). `connect()` picks up the execution role and `AWS_REGION` from
the Lambda environment. Open the connection outside the handler so warm
invocations reuse it:

```python
import ddbsh

conn = ddbsh.connect()

def handler(event, context):
    cur = conn.cursor()
    cur.execute("select * from orders where customer = ?;", [event["customer"]])
    return [dict(row) for row in cur]
```

Rows contain `Decimal` and, for B values, `bytes`; convert them before
returning JSON from a handler.

## Offline: `connect_memory`

`ddbsh.connect_memory()` returns a connection to an in-memory store in the
process (the engine's `MemoryClient`), with no AWS account. It models the data
plane, transactions and a little of the control plane (TTL, PITR, billing
mode). EXPLAIN, backups, vector search and SHOW LIMITS are not modeled: the
cursor raises `NotSupportedError` and `run` prints the engine's error. Use it
for unit tests of code that uses ddbsh.

## `python -m ddbsh`

`python -m ddbsh -c "<statements>"` (or `-f <file>`, or standard input) runs
statements through `Connection.run_result` and prints the output as the command
does, with the command's exit status. It takes `-e`, `--output` and
`--json-flavor`, and uses `DDBSH_ENDPOINT_OVERRIDE` as the command does. It
exists so the golden suite can drive the Python package the way it drives the
native binary; it is not an interactive shell (use the `ddbsh` command for
that).

## Building from source

`pip install .` in a clone builds the wheel with CMake through
scikit-build-core and nanobind. It needs the [build toolchain](BUILDING.md)
(a C++23 compiler, CMake 3.21+, bison 3.7+ and flex) and the C headers of the
Python it builds for. It does not need the AWS SDK for C++: the package holds
the SDK-free engine and the `ddbsh-wire` codec only. The CMake option behind
it is `DDBSH_BUILD_PYTHON` (default OFF).

`devtools/run-tests --python` runs the package's tests (see
[TESTING](TESTING.md#python-package-gates)).
