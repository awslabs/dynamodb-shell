<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Output formats

By default ddbsh prints rows the way the original `ddbsh` did:
`{attr: value, ...}`, one item per line. You can choose another format for
rows instead:

| Format | What it writes |
|---|---|
| `default` | ddbsh's own output, unchanged |
| `json` | one JSON array per result set |
| `csv` | RFC 4180 fields, LF line ends |
| `table` | a box drawn with `+`, `-` and `\|` per DynamoDB page |

A second setting, the JSON flavor, says how values are written: `plain` JSON
values (the default) or `dynamodb`, the typed encoding DynamoDB itself uses
(`{"N": "1"}`). `default` ignores the flavor.

The browser edition has one more format, Table (HTML); see
[configuration 3](../configurations/03-browser-aws.md#output-formats).

## Choosing a format

On the command line:

```sh
ddbsh --output json -c 'select * from ddbsh_doc_fmt where kind = "film";'
ddbsh --output json --json-flavor dynamodb -f queries.sql
```

In a script, a config file, or an interactive session, a line
`set output <format>` or `set json-flavor <flavor>` at the start of a batch
changes the format for the statements that follow:

```
set output csv
select title, year from ddbsh_doc_fmt where kind = "film" and id = 1;
go
set output json
set json-flavor dynamodb
select title, year from ddbsh_doc_fmt where kind = "film" and id = 1;
```

```
title,year
Alien,1979
[
  {"title": {"S": "Alien"}, "year": {"N": "1979"}}
]
```

Like `set terminator`, these lines are recognized only at the start of a batch
(see [isql-mode input](README.md#isql-mode-input)). A second `set output`
after a statement in the same batch is a syntax error, so end the batch with a
`go` line first, as above. An unknown value writes
`unknown output format: <x> (expected default, json, csv, table)` (or
`unknown JSON flavor: <x> (expected plain, dynamodb)`) to stderr and changes
nothing.

When more than one setting applies, the first of these wins:

1. a `set output` / `set json-flavor` line later in the session;
2. `--output` / `--json-flavor`;
3. a `set output` / `set json-flavor` line in the config file
   (`~/.ddbsh_config`, or `DDBSH_CONFIG_FILE`);
4. `default` and `plain`.

An unknown `--output` or `--json-flavor` value prints a message and the usage
line, and exits 1.

The browser edition chooses the format with its Output and JSON dropdowns;
there, `set output` and `set json-flavor` are syntax errors.

## The same SELECT in each format

The examples on this page use this table:

```sql
create table ddbsh_doc_fmt ( kind string, id number )
  primary key ( kind hash, id range );
insert into ddbsh_doc_fmt ( kind, id, title, year )
  values ( "film", 1, "Alien", 1979 );
insert into ddbsh_doc_fmt ( kind, id, title, year, genres )
  values ( "film", 2, "Heat", 1995, [ "crime", "drama" ] );
insert into ddbsh_doc_fmt ( kind, id, title, year, details )
  values ( "film", 3, "Up, Up and Away", 2009, { score: 8.3, crew: [ "A", "B" ] } );
```

and this query, which DynamoDB answers with one Query page:

```sql
select * from ddbsh_doc_fmt where kind = "film";
```

`default`:

```
{id: 1, kind: film, title: Alien, year: 1979}
{genres: [crime, drama], id: 2, kind: film, title: Heat, year: 1995}
{details: {crew:[A, B], score:8.3}, id: 3, kind: film, title: "Up, Up and Away", year: 2009}
```

`json` (plain):

```
[
  {"id": 1, "kind": "film", "title": "Alien", "year": 1979},
  {"genres": ["crime", "drama"], "id": 2, "kind": "film", "title": "Heat", "year": 1995},
  {"details": {"crew": ["A", "B"], "score": 8.3}, "id": 3, "kind": "film", "title": "Up, Up and Away", "year": 2009}
]
```

`json` with `--json-flavor dynamodb`:

```
[
  {"id": {"N": "1"}, "kind": {"S": "film"}, "title": {"S": "Alien"}, "year": {"N": "1979"}},
  {"genres": {"L": [{"S": "crime"}, {"S": "drama"}]}, "id": {"N": "2"}, "kind": {"S": "film"}, "title": {"S": "Heat"}, "year": {"N": "1995"}},
  {"details": {"M": {"crew": {"L": [{"S": "A"}, {"S": "B"}]}, "score": {"N": "8.3"}}}, "id": {"N": "3"}, "kind": {"S": "film"}, "title": {"S": "Up, Up and Away"}, "year": {"N": "2009"}}
]
```

`csv`:

```
kind,id,details,genres,title,year
film,1,,,Alien,1979
film,2,,"[""crime"", ""drama""]",Heat,1995
film,3,"{""crew"": [""A"", ""B""], ""score"": 8.3}",,"Up, Up and Away",2009
```

`table`:

```
+------+----+------------------------------------+--------------------+-----------------+------+
| kind | id | details                            | genres             | title           | year |
+------+----+------------------------------------+--------------------+-----------------+------+
| film |  1 |                                    |                    | Alien           | 1979 |
| film |  2 |                                    | ["crime", "drama"] | Heat            | 1995 |
| film |  3 | {"crew": ["A", "B"], "score": 8.3} |                    | Up, Up and Away | 2009 |
+------+----+------------------------------------+--------------------+-----------------+------+
```

## What is formatted, and what stays text

A format applies only to rows:

- the items of a `SELECT` (including vector search, which adds a last column
  named `score`, or `_score`, `__score`, … when an item already has an
  attribute of that name);
- the items of a read transaction;
- the items that `RETURN VALUES` returns from `INSERT`, `REPLACE`, `UPDATE`,
  `UPSERT` and `DELETE`;
- the reports that have columns: `SHOW TABLES`, `SHOW BACKUPS` and
  `SHOW LIMITS`.

Everything else is text, written exactly as in `default`, in every format:
status lines (`INSERT`, `UPDATE (3 read, 3 modified, 0 ccf)`, the
`SELECT (…)` consumed-capacity line), `DESCRIBE`, `SHOW CREATE TABLE`,
`SHOW VERSION`, `SHOW CONNECTION`, `EXPLAIN`, `HELP`, and errors. `DESCRIBE`
has no columns: it stays its multi-line text report.

The report columns are:

| Report | Columns |
|---|---|
| `SHOW TABLES` | `TableName`, `TableStatus`, `BillingMode`, `TableClass`, `TableId`, `TableArn`, `TimeToLive`, `GSICount`, `LSICount`, `VSICount` |
| `SHOW TABLES BRIEF` | `TableName` |
| `SHOW BACKUPS` | `TableName`, `BackupName`, `BackupStatus`, `BackupArn`, `CreationDateTime`, `ExpiryDateTime` |
| `SHOW LIMITS` | `Region`, `Limit`, `RCU`, `WCU`; one row each for `Account Max`, `Table Max`, `Total Tables`, `Total GSI` and `Total` |

A report cell holds the same string `default` shows (for example
`PAY_PER_REQUEST*`), and the counts and capacity numbers are numbers. A
`SHOW TABLES` row whose `DescribeTable` or `DescribeTimeToLive` failed has
only `TableName`.

```
$ ddbsh -q --output table -c 'show tables brief like "ddbsh_doc_fmt";'
+---------------+
| TableName     |
+---------------+
| ddbsh_doc_fmt |
+---------------+
```

## Result sets, and several statements in one run

Rows are grouped into result sets. A result set is the rows of one statement
up to the next text: a `SELECT` over many pages is one result set, and a
status line or an error closes the open one. So a run of several statements
is a sequence of result sets with the text between them. In `json` that is a
sequence of JSON documents, one array per result set, with the status lines
between them as plain text. It is not one JSON document:

```sh
ddbsh -q --output json -c 'select title from ddbsh_doc_fmt where kind = "film" and id = 1;
  insert into ddbsh_doc_fmt ( kind, id ) values ( "film", 1 );
  select title, year from ddbsh_doc_fmt where kind = "film" and id = 2;'
```

```
[
  {"title": "Alien"}
]
The conditional request failed
[
  {"title": "Heat", "year": 1995}
]
```

(The `INSERT` fails because the key exists; its error goes to stderr.) To get
one parseable JSON document, run one row-producing statement per invocation.

Output is written as it arrives. A `SELECT` writes each page's rows while the
next page is fetched, and `SHOW TABLES` writes each table's row as soon as its
`DescribeTable` returns. The `table` format is the exception: it prints one
box per page (below).

## Columns

In `csv` and the two tables, the columns of a result set come in this order:

1. For `SELECT *`, the key attributes first: the table's partition and sort
   key, then the index keys when you select from `table.index`. They are shown
   even when no row has them, so an empty result still has a header.
2. For an explicit projection (`SELECT a, b, c …`), the projected attributes,
   in your order, and nothing else. The header is fixed.
3. Then every other attribute of the first page, in byte order of the name.
4. Then, when a later page brings new attributes, those, in byte order,
   appended at the end. Columns never move or disappear within a result set.

`RETURN VALUES` items and read-transaction rows have no key hint, so their
columns start in byte order.

An explicit projection gives one fixed header:

```
$ ddbsh -q --output csv -c 'select title, year, genres from ddbsh_doc_fmt where kind = "film";'
title,year,genres
Alien,1979,
Heat,1995,"[""crime"", ""drama""]"
"Up, Up and Away",2009,
```

A `json` element is an object with its attributes in byte order, as in
`default`; a report row keeps the report's column order.

## json

- One array per result set: `[` on its own line, one element per line indented
  two spaces, `]` on its own line. An empty result set is `[]`.
- Each element is written as soon as its row arrives.
- **Plain flavor.** A string is a JSON string; a number is a JSON number with
  DynamoDB's exact digits (a number that does not fit JSON's number grammar
  is written as a string); binary is a base64 string;
  booleans are `true`/`false`; NULL is `null`; a list is an array; a map is an
  object with its keys in byte order; a string or binary set is an array of
  strings; a number set is an array of numbers.
- **DynamoDB flavor.** The typed encoding of the DynamoDB API:
  `{"S": "x"}`, `{"N": "1"}`, `{"B": "aGVsbG8="}`, `{"BOOL": true}`,
  `{"NULL": true}`, `{"L": [...]}`, `{"M": {...}}`, `{"SS": [...]}`,
  `{"NS": ["1", "2"]}`, `{"BS": [...]}`. Report rows use it too.
- **Numbers are written verbatim.** `12345678901234567890.123456789` stays
  `12345678901234567890.123456789` in plain JSON (and `{"N": "12345678901234567890.123456789"}`
  in the DynamoDB flavor). A program that reads the plain JSON into
  floating-point numbers may lose digits; read the DynamoDB flavor to keep
  them as strings.
- Strings escape `"`, `\` and the control characters U+0000 to U+001F;
  other UTF-8 is written as is.

## csv

- Comma-separated, one line per row, with LF line ends (RFC 4180 specifies
  CRLF). A field is quoted when it contains a comma, a double quote, a CR or an
  LF, and a double quote inside it is doubled.
- A missing attribute is an empty field. Strings and numbers are written as
  they are, binary as base64, booleans as `true`/`false`, NULL as `null`, and
  a map, list or set as JSON in the chosen flavor.
- The header is written when the result set opens. When a later page brings
  attributes the header lacks, ddbsh writes a new header line first, with the
  earlier columns in the same positions and the new ones at the end. One page,
  or pages that share their attributes, give an ordinary CSV file with one
  header. Here a bulk `UPDATE … RETURN VALUES ALL NEW` returns three items one
  at a time, and each of the last two brings a new attribute:

  ```
  $ ddbsh -q --output csv -c 'update ddbsh_doc_fmt set seen = TRUE where kind = "film" and id < 9 return values all new;'
  id,kind,seen,title,year
  1,film,true,Alien,1979
  id,kind,seen,title,year,genres
  2,film,true,Heat,1995,"[""crime"", ""drama""]"
  id,kind,seen,title,year,genres,details
  3,film,true,"Up, Up and Away",2009,,"{""crew"": [""A"", ""B""], ""score"": 8.3}"
  UPDATE (3 read, 3 modified, 0 ccf)
  ```

  Use an explicit projection when you need exactly one header.
- A string's bytes are written raw, a NUL byte included.
- Cells are not changed to defend against spreadsheet formula injection: a
  value that starts with `=`, `+`, `-` or `@` is written as it is. Opening the
  file in a spreadsheet program is at your own risk.

## table

- One box per DynamoDB page, because a box needs every column width before
  its first line (the precedent is psql's `FETCH_COUNT`). A `SELECT` over two
  pages prints two boxes, with the same columns plus any the second page
  added.
- Numbers are right-aligned; everything else is left-aligned.
- Column widths count Unicode code points, so wide characters (for example
  CJK) and combining characters misalign the box.
- Control characters in a cell (U+0000 to U+001F, and DEL) are shown as
  escapes (`\n`, `\t`, `\u0000`, ...), so a newline never breaks a box.
- An empty result with known columns prints a header-only box.
- A bulk `UPDATE` or `DELETE` with `RETURN VALUES` holds the returned items
  and prints them as one box just before the statement's accounting line:

  ```
  $ ddbsh -q --output table -c 'update ddbsh_doc_fmt set seen = FALSE where kind = "film" and id < 9 return values all new;'
  +----+------+-------+-----------------+------+--------------------+------------------------------------+
  | id | kind | seen  | title           | year | genres             | details                            |
  +----+------+-------+-----------------+------+--------------------+------------------------------------+
  |  1 | film | false | Alien           | 1979 |                    |                                    |
  |  2 | film | false | Heat            | 1995 | ["crime", "drama"] |                                    |
  |  3 | film | false | Up, Up and Away | 2009 |                    | {"crew": ["A", "B"], "score": 8.3} |
  +----+------+-------+-----------------+------+--------------------+------------------------------------+
  UPDATE (3 read, 3 modified, 0 ccf)
  ```

- `SHOW TABLES` fills in one box per `ListTables` page (up to 100 tables), as
  each page's tables have been described. In the other formats each table's
  row appears as soon as it is described. `SHOW LIMITS` is one box.

## Edge cases

- **An empty result.** `json` writes `[]`; `csv` writes the header of the key
  columns (`kind,id` for the example table); `table` writes a header-only box.
- **A read transaction's miss.** A `SELECT` in a read transaction that finds no
  item keeps its position as an empty row: `{}` in `json`, an empty row (all
  fields empty) in `csv` and `table`:

  ```
  $ ddbsh -q --output json -c 'begin;
    select title, year from ddbsh_doc_fmt where kind = "film" and id = 1;
    select title, year from ddbsh_doc_fmt where kind = "film" and id = 99;
    commit;'
  [
    {"title": "Alien", "year": 1979},
    {}
  ]
  ```

  When every read misses, the result set has no columns: `json` writes one
  `{}` per miss, `csv` one empty line per miss, and `table` nothing.
- **`REPLACE … RETURN VALUES ALL OLD` on a new key.** There was no old item,
  so `json` writes an array with one empty element after the `REPLACE` token:

  ```
  $ ddbsh -q --output json -c 'replace into ddbsh_doc_fmt ( kind, id, title ) values ( "film", 9, "Tmp" ) return values all old;'
  REPLACE
  [
    {}
  ]
  ```

- **Sets and binary.** `default` keeps the original's `Unknown Type` for
  sets and binary values. The other formats show them: binary as base64, sets
  as JSON arrays (or `{"SS": [...]}` and the like in the DynamoDB flavor).
