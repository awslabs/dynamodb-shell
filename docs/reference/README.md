<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Command reference

ddbsh accepts SQL-like statements terminated by `;`. This reference describes
the full command surface, grouped by family:

- **[Control commands](control.md)** — `CONNECT`, `SHOW VERSION`, `QUIT`.
- **[Data Definition Language](ddl.md)** — `CREATE`/`DROP`/`ALTER TABLE`,
  `DESCRIBE`, `BACKUP`/`RESTORE`, `SHOW TABLES`/`LIMITS`/`BACKUPS`/`CREATE
  TABLE`, `EXPLAIN`.
- **[Data Manipulation Language](dml.md)** — `SELECT`, `INSERT`, `REPLACE`,
  `UPDATE`, `UPSERT`, `DELETE`, `EXPLAIN`.
- **[DML — special topics](special-topics.md)** — the `WHERE` clause, rate
  limiting, return values, on-demand throughput, warm throughput.

The rest of this page summarizes the supported command set, the isql-mode input
model, syntax-error diagnostics, and the CLI flags/config/environment.

## Supported commands

The engine drives this command set end to end against real DynamoDB:

- `CONNECT`, `QUIT` — control.
- `CREATE TABLE`, `DROP TABLE` — DDL (on-demand default, waiters unless
  `NOWAIT`, `IF [NOT] EXISTS`).
- `DESCRIBE <table>`, `SHOW TABLES` — introspection.
- `INSERT` / `REPLACE` — PutItem (INSERT conditional on
  `attribute_not_exists(pk)`; REPLACE unconditional).
- `UPDATE` / `UPSERT` — UpdateItem: a single conditional write when the full
  base-table key is given (UPDATE prepends `attribute_exists(pk)`, UPSERT does
  not), else a Query/Scan enumeration + per-item write. `SET` (plain value,
  attribute copy, arithmetic `+`/`-`, `if_not_exists(path, value)`) and
  `REMOVE`. Reports `(n read, m modified, k ccf)`; a conditional-check failure
  is counted as a `ccf`, not an error.
- `DELETE` — DeleteItem with the same single-vs-enumerate dispatch; no `WHERE`
  deletes every item. Reports `(n read, m modified, k ccf)`.
- `SELECT` — the full SQL→API decision matrix (GetItem vs Query vs Scan, base
  table and `.index`), the WHERE expression tree, `CONSISTENT`, projections, and
  `RETURN CONSUMED CAPACITY`.
- `ALTER TABLE` — billing mode / throughput, GSI create/update/drop, stream,
  table class, SSE, deletion protection, warm throughput, `SET TTL`, `SET PITR`,
  and replica updates (UpdateTable / UpdateTimeToLive / UpdateContinuousBackups).
- Transactions — `BEGIN .. COMMIT|ROLLBACK` mapping to TransactGetItems (all
  reads, each reducible to a GetItem) or TransactWriteItems
  (Put/Update/Delete/ConditionCheck).
- Backups and restore — `BACKUP TABLE ... CALLED`, `DROP BACKUP`,
  `DESCRIBE BACKUP`, `RESTORE ... FROM BACKUP`, `RESTORE ... TO PITR`.
- `SHOW LIMITS`, `SHOW BACKUPS`, `SHOW CREATE TABLE` — introspection.
- Return-values display (`RETURN ...`) and rate limiting (`WITH RATELIMIT`),
  including the token-bucket throttle on paginated read/write enumeration.
- `SHOW VERSION`.
- `EXPLAIN <any supported command>` — prints the serialized API request(s)
  instead of executing.

**Extensions (opt-in, additive):**

- Multi-key (composite) Global Secondary Indexes — a GSI key role may name up
  to 4 partition and 4 sort attributes. A role with 2+ attributes is written as
  a parenthesized, comma-separated group followed by the role word; a
  single-attribute key stays bare. List order is composite order. Example:

  ```sql
  CREATE TABLE t ( a string, b string, c string, d string, x string )
    PRIMARY KEY ( x hash )
    GSI ( g ON ( (a, b) hash, (c, d) range ) PROJECTING ALL );
  ```

  The same `(list) role` / bare-single syntax applies to `ALTER TABLE ... CREATE
  GSI`. A `SELECT`/`UPDATE`/`DELETE` against such an index plans as a Query when
  every partition-key attribute has an `=` condition and the sort-key conditions
  form a gap-free left-to-right prefix (a range operator allowed only on the
  last specified sort key); otherwise it degrades to a Scan. Composite keys are
  a GSI-only feature: a composite key on a base-table primary key or an LSI is
  rejected client-side with a clear error.

  **SELECT planning against a composite GSI (Query vs Scan).** The rule in full:
  a **Query** requires an `=` condition on **every** partition-key component; the
  sort-key conditions must form a gap-free left-to-right prefix (no skipping),
  each `=` except the last, which may be a range operator. Any missing
  partition-key component, a gap in the sort-key prefix, or a range operator on a
  non-last sort-key component demotes the plan to a **Scan** with the predicate
  as a `FilterExpression`. Given:

  ```sql
  CREATE TABLE foo ( a number, b number, c number, d number, e number )
    PRIMARY KEY ( a hash )
    GSI ( firstgsi ON ( (a, b, c) hash, (d, e) range ) PROJECTING ALL );
  ```

  the composite HASH is `(a, b, c)` and the composite RANGE is `(d, e)`:

  | Predicate | API | Why |
  |---|---|---|
  | `a = 3` | Scan | partial composite hash → FilterExpression |
  | `a = 3 and b = 4` | Scan | partial composite hash → FilterExpression |
  | `a = 3 and b = 4 and c = 5` | Query | full composite hash → KeyConditionExpression |
  | `a = 3 and b = 4 and c = 5 and d = 6` | Query | full hash + leading range component |

  A partial-hash predicate becomes a Scan with a `FilterExpression`:

  ```
  us-east-1> explain select * from foo.firstgsi where a = 3;
  Scan({
     "TableName":   "foo",
     "IndexName":   "firstgsi",
     "ReturnConsumedCapacity":   "NONE",
     "FilterExpression":   "#aaaa1 = :vaaa1",
     "ExpressionAttributeNames":   {
        "#aaaa1":   "a"
     },
     "ExpressionAttributeValues":   {
        ":vaaa1":   {
           "N":   "3"
        }
     },
     "ConsistentRead":   false
  })
  ```

  A full-hash predicate becomes a Query with a `KeyConditionExpression`:

  ```
  us-east-1> explain select * from foo.firstgsi where a = 3 and b = 4 and c = 5;
  Query({
     "TableName":   "foo",
     "IndexName":   "firstgsi",
     "ConsistentRead":   false,
     "ReturnConsumedCapacity":   "NONE",
     "KeyConditionExpression":   "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2 AND #aaaa3 = :vaaa3",
     "ExpressionAttributeNames":   {
        "#aaaa1":   "a",
        "#aaaa2":   "b",
        "#aaaa3":   "c"
     },
     "ExpressionAttributeValues":   {
        ":vaaa1":   {
           "N":   "3"
        },
        ":vaaa2":   {
           "N":   "4"
        },
        ":vaaa3":   {
           "N":   "5"
        }
     }
  })
  ```

- Vector search — a table may carry one or more **vector indexes** for
  real-time approximate-nearest-neighbor search. The CREATE/ALTER clause lead
  accepts three synonyms: `VSI (...)`, `VECTOR (...)`, or `VECTOR INDEX (...)`.
  A vector index names the list-of-number *vector attribute* with `ON <attr>`
  (the vector attribute is NOT declared in the top attribute list), its
  `DIMENSIONS`, a `DISTANCE` function (`COSINE | EUCLIDEAN | DOT_PRODUCT`), an
  optional `SEARCH KEY` schema (`attr HASH` / `attr FILTER`), and a projection:

  ```sql
  CREATE TABLE docs ( id string, category string )
    PRIMARY KEY ( id hash )
    VSI ( embidx ON embedding DIMENSIONS 1536 DISTANCE COSINE
          SEARCH KEY ( category FILTER ) PROJECTING ALL );

  ALTER TABLE docs ( category string )
    CREATE VSI embidx ON embedding DIMENSIONS 1536 DISTANCE COSINE
      SEARCH KEY ( category FILTER ) PROJECTING ALL;
  ALTER TABLE docs DROP VSI embidx;   -- create/drop only; no in-place update
  ```

  A vector attribute value is written as a bare numeric list; INSERT/UPDATE
  store it as a DynamoDB list of numbers:

  ```sql
  INSERT INTO docs ( id, embedding ) VALUES ( "d1", [0.12, -0.34, 0.56] );
  UPDATE docs SET embedding = [0.9, 0.1] WHERE id = "d1";
  ```

  A `SELECT ... FROM <table>.<vectorindex> NEAREST <n> TO [ ... ]` plans to a
  **SearchVectors** call (never GetItem/Query/Scan): `<n>` is the TopK, the
  bracketed numeric list is the query vector (its length must equal the index
  `DIMENSIONS`), and a `WHERE` clause becomes the search filter (it may
  reference only the index's HASH / FILTER search-key attributes). Each result
  row is shown with a trailing ` score <n>` (the similarity score the service
  returns for that match):

  ```sql
  SELECT * FROM docs.embidx WHERE category = "news" NEAREST 5 TO [0.1, 0.2, 0.3];
  ```

  **What a result row contains.** Each match carries the attributes named in the
  SELECT projection plus a trailing similarity ` score <n>`. The stored vector
  is treated like any other attribute: `SELECT *` omits it by default (no
  `ProjectionExpression` is sent, and the service does not include the vector
  unless asked), but **naming the vector attribute in the projection returns it**
  — in the same `SearchVectors` call, at no extra API cost:

  ```sql
  -- vector omitted (SELECT * does not request it)
  SELECT * FROM docs.embidx NEAREST 3 TO [0.1, 0.2, 0.3];

  -- vector returned: name it in the projection
  SELECT id, category, embedding FROM docs.embidx NEAREST 3 TO [0.1, 0.2, 0.3];
  ```

## isql-mode input

The CLI assembles input in the **Sybase isql style**: it buffers physical lines
until it reads a **terminator line**, then hands the whole batch to the engine,
which parses every `;`-terminated statement in order (a missing trailing `;` on
the last statement is tolerated). Statements inside a batch stay `;`-terminated;
the terminator word itself is never sent to the engine.

- The default terminator is the word `go` on a line by itself (case-insensitive).
- `-t[STR]` / `--terminator[=STR]` overrides the terminator word. `-t` with **no
  argument** makes a **blank line** the terminator.
- The runtime meta-command `set terminator [STR]` changes the terminator
  mid-session (recognized only at the start of a batch; `set terminator` with no
  value restores the blank-line terminator).
- **EOF is always a terminator** — a file given with `-f`, the `-c` string, or an
  interactive Ctrl-D flushes the buffered batch regardless.

```
us-east-1> SELECT * FROM Movies
       ...> WHERE year = 2014;
       ...> go
```

To terminate with a **blank line** instead of the word `go`, start the shell
with `-t` (no argument), or run `set terminator` with no value mid-session:

```
$ ddbsh -t
us-east-1> SELECT * FROM Movies WHERE year = 2014;
       ...>
```

A custom terminator word is set with `-t STR` (e.g. `-t ;;`) or `set terminator
STR`.

This isql batch input is a **deliberate, documented divergence** (tenets 2 and
8) from the original `ddbsh`, which executed each statement as soon as it read a
bare unquoted `;` interactively. The divergence is additive and lives only at
the CLI layer: the engine still parses `;`-terminated statements, so the golden
`.exp` command strings (all `;`-terminated and flushed by EOF) produce
byte-identical output.

## Syntax error diagnostics

A syntax error reports the `[line, column]` location and the offending token,
and — below the offending input line — a compiler-style caret (`^`) pointing at
the exact error column:

```
us-east-1> create table foo ( a number ) primary key ( a hash ) gsi ( g on (a, b) hash, (c) range) projecting all ))
[1, 68] syntax error at ,
create table foo ( a number ) primary key ( a hash ) gsi ( g on (a, b) hash, (c) range) projecting all ))
                                                                  ^
```

(Here the GSI key schema needs its own outer parens around the role list:
`g ON ( (a, b) hash, (c) range )`.) The caret is an additive diagnostic; the
`[line, column] syntax error at <token>` line is unchanged.

## CLI flags, config, and environment

```
Usage: ddbsh [-q] [-e] [-d] [-t[TERM]] [--ca-bundle <file>] [--show-retries] [-c "<cmds>" | -f <file>]
```

- `-c "<cmds>"` — run a batch string, then exit (mutually exclusive with `-f`).
- `-f <file>` — run a file, then exit (mutually exclusive with `-c`).
- `-q` — quiet: suppress the startup banner.
- `-e` — abort on the first error.
- `-d` — debug. Accepted for flag-surface parity with the original `ddbsh`, but
  currently a no-op/reserved: the engine has no logging subsystem yet, so the
  flag sets nothing. It is retained so scripts that pass `-d` keep working and
  so the behavior is wired in a future phase without a CLI surface change
  (`TODO(phase-N)`).
- `-t[STR]` / `--terminator[=STR]` — set the batch terminator (see above).
- `--ca-bundle <file>` — trust a custom/self-signed TLS CA bundle (PEM) for the
  DynamoDB endpoint, mirroring the AWS CLI's `--ca-bundle`. CA-bundle trust has
  three sources, in precedence order: this flag, then the `AWS_CA_BUNDLE`
  environment variable, then the resolved profile's `ca_bundle` config setting.
  The resolved path is mapped to the SDK client's `caFile`. Needed to reach an
  endpoint that serves a self-signed certificate — for example a local ExtendDB
  server. The trust setting is preserved across a `CONNECT` to another
  region/endpoint. The full CA-bundle story (all three sources, the
  `aws configure set/get ca_bundle --profile` commands, and fail-closed TLS) is
  documented in
  [configuration 2 — native ddbsh against a local ExtendDB server](../configurations/02-native-extenddb.md).
- `--show-retries` — write one line to stderr for every retry the AWS SDK makes
  when DynamoDB throttles a request or a request fails with a transient error,
  before the SDK waits out its backoff. Off by default; stdout is unchanged
  either way. A line reads
  `ddbsh: Scan throttled (ProvisionedThroughputExceededException); attempt 2 of 3 in 150 ms`.
  "throttled" becomes "failed" for an error that is not a throttle; the error
  code, the `of N` budget and the `in N ms` delay are left out when the SDK
  does not supply them. Nothing is printed for an error the SDK will not retry.

At startup the CLI loads a config file (`DDBSH_CONFIG_FILE`, else
`~/.ddbsh_config`) if present and reads/writes interactive history at
`~/.ddbsh_history`. The region resolves from `AWS_REGION`, then
`AWS_DEFAULT_REGION`, then `us-east-1`; `DDBSH_ENDPOINT_OVERRIDE` sets an
endpoint override, which marks the prompt with a `(*)` suffix.

