<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Embedding the library

The embedding API is a simple, ODBC / Sybase-DB-Library-style C++ surface:
**open a connection, submit a statement, walk result sets, walk rows, read a
typed column.** You open one handle and work through it — no four-object
ceremony, no variant switching, no separate renderer in your code.

### Embedding against real DynamoDB (`ddbsh::aws::open`)

`ddbsh::aws::open(region)` returns a ready-to-use `Database`: it resolves
region/endpoint/credentials through the standard AWS chain, manages the AWS SDK
lifecycle internally (you never write `AwsApiGuard`), builds the shared
`AwsDDBClient`, and owns a `ddbsh::Connection` over it. Consume it via
`find_package(ddbsh)`:

```cmake
find_package(ddbsh CONFIG REQUIRED)        # defines ddbsh::ddbsh and ddbsh::aws
find_package(AWSSDK REQUIRED COMPONENTS dynamodb)
target_link_libraries(myapp PRIVATE ddbsh::aws)   # pulls ddbsh::ddbsh + the SDK
```

```cpp
#include "ddbsh/aws/database.hpp"

auto db = ddbsh::aws::open("us-east-1");             // ONE object
if (!db) { std::fputs(db.error().message.c_str(), stderr); return 1; }

// The single-SELECT shortcut: iterate typed, null-aware rows directly.
auto rows = db->query("SELECT * FROM Movies WHERE id = \"m1\";");
if (!rows) { std::fputs(rows.error().message.c_str(), stderr); return 1; }
for (const ddbsh::Row& row : *rows) {
    std::optional<std::string> title = row.get_string("title");
    std::optional<long long>   year  = row.get_int("year");
    // ...
}
```

Any statement (or a batch, or a multi-row write) returns a `Results` you walk
DB-Library-style — `next_result()` steps result sets, `next_row()` steps rows,
`column()` reads a typed `Cell`:

```cpp
auto res = db->exec("INSERT INTO t (id) VALUES (1),(2); SELECT * FROM t;");
if (!res) { std::fputs(res.error().message.c_str(), stderr); return 1; }
ddbsh::Results& r = *res;
do {
    switch (r.kind()) {
    case ddbsh::ResultKind::rows:
        while (r.next_row()) use(r.column("id").as_string());
        break;
    case ddbsh::ResultKind::status: note(r.status_token()); break;   // "INSERT"
    case ddbsh::ResultKind::write:  account(r.read(), r.modified(), r.ccf()); break;
    case ddbsh::ResultKind::report: for (auto& ln : r.lines()) show(ln); break;
    case ddbsh::ResultKind::explain: show(r.explain_text()); break;
    case ddbsh::ResultKind::empty: break;
    }
} while (r.next_result());
if (auto e = r.error()) std::fputs(e->message.c_str(), stderr);  // per-statement failure
```

A `Cell` separates the three DynamoDB states (`missing()` / `is_null()` / a
typed value) and offers `as_string` / `as_int` / `as_double` / `as_bool`. To
reproduce **byte-exact ddbsh output** (what the shell prints), call
`r.render()` (current result set) or `r.render_all()` (the whole batch).

**Prepared / parameterized statements (injection-safe).** `prepare()` parses a
single statement ONCE; bind its positional `?` parameters (1-based, by order of
appearance), then `exec`/`query` it — and re-bind and re-run without re-parsing
(ODBC `SQLPrepare` / Sybase DB-Library shape). Binding is *typed*: a bound value
flows straight into the request, so you never concatenate, and you can bind any
value type — including sets and binary, which have no literal syntax — via the
`Value` factories:

```cpp
auto stmt = db->prepare("SELECT * FROM Movies WHERE id = ? AND year = ?;");
if (!stmt) { /* parse error */ }
stmt->bind(1, std::string("m1")).bind(2, 2010LL);
auto rows = db->query(*stmt);

// Re-execute with different bindings — no re-parse.
stmt->bind(1, std::string("m2")).bind(2, 2020LL);
auto more = db->query(*stmt);

// Set / binary binds (no ddbsh literal syntax exists for these):
auto ins = db->prepare("INSERT INTO Movies ( id, genres, poster ) VALUES ( ?, ?, ? );");
ins->bind(1, std::string("m3"))
   .bind(2, ddbsh::Value::string_set({"scifi", "thriller"}))
   .bind(3, ddbsh::Value::binary_value("aGVsbG8="));  // base64 bytes
auto r = db->exec(*ins);
```

Markers are positional `?` only (the ODBC-canonical marker). Named `:name`
markers are intentionally not offered: this grammar accepts the no-space
`{key:value}` map form, which would make a `:name` marker ambiguous. An unbound
parameter — or a bare `?` used outside the prepared path — is reported as an
error, never silently executed.

**Client configuration (timeouts, retry).** The `OpenOptions` struct form of
`open()` passes a full `Aws::Client::ClientConfiguration` through (timeouts,
retry strategy, max connections, proxy); ddbsh overlays region/endpoint and its
own user-agent. A separate `waiter_timeout` bounds the CREATE/DROP waiters.

**Watching retries.** When DynamoDB throttles a request, or a request fails with
a transient error, the AWS SDK backs off and sends it again; the engine never
sees those retries. To watch them, register a `ddbsh::RetryObserver`
(`ddbsh/retry_event.hpp`) with `db->connection().set_retry_observer(&observer)`.
Its `on_retry(const ddbsh::RetryEvent&)` is called on the requesting thread
once per retry the SDK has decided to make, before it sleeps. The event carries
the operation (`"Scan"`), the attempt about to be made (2 for the first retry),
the SDK's attempt budget, the error code, whether it is a throttle, and the
delay about to be slept. No event is raised for an error the SDK will not retry
or for the last attempt. The pointer is borrowed: keep the observer alive until
you pass `nullptr` or close the connection, and until no `on_retry` call is
still running. Clearing the observer does not wait for an `on_retry` call
already running on another thread. The registration survives
`reconnect()` and `CONNECT`. `IDDBClient::set_retry_observer` is the same hook
for an embedder that owns its client; `ddbsh::is_throttling_error(code)` is the
throttling-code test the events use.

**Header boundary:** `ddbsh/*.hpp` (including `ddbsh/connection.hpp`) are
SDK-free (compile with only a C++23 toolchain); `ddbsh/aws/*.hpp` require the
AWS SDK for C++. Link `ddbsh::ddbsh` for the engine alone, `ddbsh::aws` for the
ready-made AWS client and `open()`.

`examples/ddbshlib_sample/` is a complete, runnable live program: `open()`,
`CREATE` a uniquified on-demand table, `INSERT`, `SELECT` three ways
(GetItem / Query / Scan), `DROP`, self-cleaning on every exit path via an RAII
guard. It builds as an external `find_package(ddbsh)` consumer via its own
`CMakeLists.txt`. Because it spends, its CTest (`ddbshlib_sample`) and the
real-DynamoDB functional test (`ddbsh_live_api`) are OFF by default; enable them
with `-DDDBSH_TEST_LIVE_SAMPLE=ON` where credentials are present.

### Embedding offline (`ddbsh::testing::MemoryClient`)

A `ddbsh::Connection` works over ANY `IDDBClient`, so you can drive the exact
same embedding API with **no AWS account, no credentials, no network, zero
cost** against the in-memory `ddbsh::testing::MemoryClient` — ideal for
unit-testing your own ddbsh usage:

```cpp
#include "ddbsh/connection.hpp"
#include "ddbsh/testing/memory_client.hpp"

ddbsh::testing::MemoryClient client;                 // genuine in-memory store
ddbsh::Connection db(ddbsh::SessionConfig{"local", ""}, client);
db.exec("create table t ( id string ) primary key ( id hash );");
db.exec("insert into t ( id, v ) values ( \"a\", 1 );");
for (const ddbsh::Row& r : *db.query("select * from t where id = \"a\";"))
    use(r.get_string("v"));
```

`MemoryClient` is SDK-free and links `ddbsh::ddbsh` alone. It models the
primary-key data plane (GetItem / Query / Scan / Put / Update / Delete),
evaluates the `FilterExpression`s the engine emits, offers opt-in bounded paging
(`set_page_size`), runs transactions, and records enough control-plane state for
an `ALTER` → `DESCRIBE` round-trip; backups/restore, vector search, and the full
control-plane report formatting are not modeled (see the header for the exact
scope). `examples/ddbshlib_inmemory_sample/` is a complete
runnable version, built as a default-on, credential-less CTest
(`ddbshlib_inmemory_sample`). For a tour of the Phase-5 embedding features —
prepared statements, typed set/binary bind, lazy streaming, and transactions —
see `examples/ddbshlib_features_sample/`, also offline over `MemoryClient` and
built as a default-on CTest (`ddbshlib_features_sample`). See
[`examples/README.md`](../examples/README.md).

> **Advanced / injection interface.** The lower-level engine SPI — `Session`,
> `IDDBClient`, `ExecOutcome`, `OutcomeSink`, and `ddbsh::render::Renderer` —
> remains public for embedders who need the raw fragment stream or who inject
> their own backend. Most applications want the `Connection`/`Database` surface
> above.
>
> `Session::execute(stmt, sink)` hands each fragment to `OutcomeSink::emit` as
> soon as the DynamoDB response that completes it arrives: one per `SELECT`
> page, per written `INSERT` row, per `SHOW TABLES` row, per `SHOW BACKUPS`
> page, per `DESCRIBE` or `SHOW LIMITS` response group, per returned item of a
> bulk `UPDATE`/`DELETE … RETURN VALUES`, and per request group of a bulk
> `EXPLAIN UPDATE`/`EXPLAIN DELETE` (each page's read request, then that page's
> write requests). Fragments emitted before a later failure stay emitted; the
> returned `Error` follows them. After the last fragment, on success or
> failure, the session calls `OutcomeSink::end_statement()` once (the default
> does nothing); if the sink's own `emit` throws, the exception propagates and
> `end_statement()` is not called. A custom
> `IDDBClient` can override `list_tables_page` so `SHOW TABLES` streams page by
> page; the default reads the whole list with `list_tables` first. The
> buffering `Session::execute(stmt)` still returns one outcome per command.

### Streaming output and the output formats

`Session::execute(statement, sink)` (and `Session::run(text, sink)`, which
parses first) is the streaming entry point. It calls `sink.emit(fragment)` for
each fragment as the DynamoDB response that completes it arrives, in output
order, then calls `sink.end_statement()` exactly once, on success and on
failure, and then returns `Result<void>`. Fragments emitted before a failure
stay emitted, and the returned `Error` comes after them. Some fragments carry
no rows: an empty `SELECT` emits one `SelectOutcome` with no items (it still
carries the `column_hint`), and `SHOW TABLES` / `SHOW BACKUPS` emit an empty
`ReportOutcome` to mark a page end that a `LIKE` filter emptied, or an empty
result. The `Renderer` renders those as nothing, so a sink that counts
fragments as content should expect them.

The `ddbsh::render` format API (`ddbsh/format.hpp`, SDK-free) gives the same
output formats the CLI's `--output` does (see the
[output formats reference](reference/output-formats.md)):

- `FormattingSink(FormatOptions, Writer)` is an `OutcomeSink` that formats each
  fragment as it arrives and passes the bytes to your writer, tagged
  `Stream::out` or `Stream::err`. `FormatOptions` holds an `OutputFormat`
  (`standard`, `json`, `csv`, `table`) and a `JsonFlavor` (`plain`,
  `dynamodb`). `standard` writes exactly what the `Renderer` writes.
  `set_options()` changes the format between statements.
- `ResultSetSink` is the base class that owns the one-open-result-set rule:
  rows open a result set (or continue the open one), text closes it first, and
  `end_statement()` closes it. Derive from it when you lay out rows yourself
  (the browser's HTML table does), and implement `on_open`, `on_rows`,
  `on_close` and `on_text`.
- `decompose(outcome)` splits one outcome into `TextPart`s (bytes that stay
  text in every format) and `RowsPart`s (rows plus the columns known before
  them).
- `ColumnTracker` keeps one result set's column list: it only appends, and
  `add()` returns the index of the first column it added.
- `format_cell(value, flavor)` is the text of one CSV or table cell, and
  `append_json_value(out, value, flavor)` writes one value as JSON.
- `render_formatted(outcome, options)` formats one buffered outcome as a whole.

This program writes the rows of a `SELECT` as JSON over the in-memory client:

```cpp
#include <cstdio>
#include <string_view>

#include "ddbsh/format.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/testing/memory_client.hpp"

int main() {
    ddbsh::testing::MemoryClient client;
    ddbsh::Session session(ddbsh::SessionConfig{"local", ""}, client);

    // Every piece of output goes to the writer as soon as it is formatted.
    ddbsh::render::FormattingSink sink(
        {ddbsh::render::OutputFormat::json, ddbsh::render::JsonFlavor::plain},
        [](ddbsh::render::Stream stream, std::string_view text) {
            std::FILE* f = stream == ddbsh::render::Stream::out ? stdout : stderr;
            std::fwrite(text.data(), 1, text.size(), f);
        });

    const ddbsh::render::Renderer renderer;  // for the error text
    for (std::string_view text : {
             "create table t ( id string ) primary key ( id hash );",
             "insert into t ( id, v ) values ( \"a\", 1 ), ( \"b\", 2 );",
             "select * from t;",
         }) {
        // run() parses, executes, emits each fragment to the sink, and calls
        // sink.end_statement() before it returns, so the JSON array is closed
        // before any error is printed.
        if (auto done = session.run(text, sink); !done) {
            std::fputs(renderer.render_error(done.error()).c_str(), stderr);
        }
    }
}
```

It prints:

```
CREATE
INSERT
INSERT
[
  {"id": "a", "v": 1},
  {"id": "b", "v": 2}
]
```

A `FormattingSink` is not thread-safe: use one per session and one statement
at a time.

#### Getting the AWS SDK for C++

The `ddbsh::aws` component and the CLI need the AWS SDK for C++ (`dynamodb`,
`sts`, `iam` components, version ≥ 1.11.916 for vector search):

- **macOS:** `brew install aws-sdk-cpp` (see
  [Avoiding AWS SDK version conflicts](BUILDING.md#avoiding-aws-sdk-version-conflicts) if
  you have more than one SDK installed).
- **Linux:** a distribution package where available, or build from source
  (<https://github.com/aws/aws-sdk-cpp>); point CMake at a custom install with
  `-DCMAKE_PREFIX_PATH=/prefix/of/aws-sdk-cpp`.
- **Windows:** build from source and add the install prefix to
  `CMAKE_PREFIX_PATH`.

The engine library alone (`ddbsh::ddbsh`, `DDBSH_BUILD_AWS=OFF`) needs no AWS
SDK — the offline `ddbshlib_inmemory_sample` builds against just that.

