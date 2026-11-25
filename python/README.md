<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# ddbsh for Python

[ddbsh](https://github.com/awslabs/dynamodb-shell), the DynamoDB shell, as a
Python library. Write SQL-like statements; ddbsh turns each one into the
cheapest DynamoDB call (GetItem, Query or Scan) and returns the rows.

The package contains the same C++ engine the `ddbsh` command uses, so a
statement makes the same DynamoDB requests and prints the same text. Requests
are signed and sent with botocore, so credentials, regions, profiles and roles
work as they do in boto3 (including AWS Lambda).

```
pip install ddbsh
```

## Text, exactly as the command prints it

```python
import ddbsh

conn = ddbsh.connect()                      # the default boto3 session
print(conn.run("select * from movies where id = 'm1';"))
print(conn.run("select * from movies;", format="json"))
```

## A DB-API 2.0 cursor

```python
cur = conn.cursor()
cur.execute("select * from movies where id = ? and yr = ?;", ["m1", 2010])
for row in cur:                             # rows are dicts
    print(row["title"])                     # N values are decimal.Decimal

cur.execute("update movies set rating = ? where id = ?;", [5, "m1"])
print(cur.write_result)                     # WriteResult(verb='UPDATE', ...)
```

## Offline

```python
conn = ddbsh.connect_memory()               # an in-memory store, no AWS
conn.run("create table t ( id string ) primary key ( id hash );")
```

The full guide (connections, credentials, Lambda, the type mapping, errors,
threads) is
[docs/PYTHON.md](https://github.com/awslabs/dynamodb-shell/blob/main/docs/PYTHON.md).

Licensed under the Apache License 2.0.
