# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""The Python package against real DynamoDB (devtools/run-python-golden).

WARNING: this talks to a real AWS account. It creates and drops the tables
ddbsh_py_live and, in the account of the AWS_PROFILE `cross-acct`,
ddbsh_test_table_1, which costs money. The offline suite is python/tests; this
directory is run only by the live gate, never by a plain `pytest`.
"""

from __future__ import annotations

from decimal import Decimal

import boto3
import pytest

import ddbsh

TABLE = "ddbsh_py_live"
# The cross-account family's table: CI's cross-account role may use no other
# table (docs/CI.md, the accounts and roles).
# The golden suite's cleanup drops it too.
CROSS_TABLE = "ddbsh_test_table_1"


@pytest.fixture(scope="module")
def conn() -> ddbsh.Connection:
    c = ddbsh.connect()
    c.run(f"drop table if exists {TABLE};")
    out = c.run(
        f"create table {TABLE} ( pk string, sk number ) primary key ( pk hash, sk range );"
    )
    assert out == "CREATE\n", out
    yield c
    assert c.run(f"drop table if exists {TABLE};") == "DROP\n"
    c.close()


def test_target_is_real_dynamodb(conn: ddbsh.Connection) -> None:
    text = conn.run("show connection;")
    assert "Backend: wire/botocore " in text
    assert "-> https://dynamodb." in text and ".amazonaws.com (default)" in text
    assert not conn.endpoint_overridden


def test_types_round_trip(conn: ddbsh.Connection) -> None:
    cur = conn.cursor()
    item = {
        "s": "a b, c",
        "n": Decimal("12345678901234567890.123456789"),
        "b": b"\x00\xffbin",
        "t": False,
        "nul": None,
        "l": [1, "x", None, True, [b"\x01"]],
        "m": {"k": {"j": [Decimal("-1.5")]}},
        "ss": {"x", "y"},
        "ns": {1, 2, Decimal("3.5")},
        "bs": {b"\x01", b"\x02"},
    }
    names = ", ".join(item)
    marks = ", ".join("?" for _ in item)
    cur.execute(
        f"insert into {TABLE} ( pk, sk, {names} ) values ( ?, ?, {marks} );",
        ["types", 1, *item.values()],
    )
    assert cur.status == "INSERT"
    cur.execute(f"select * from {TABLE} where pk = ? and sk = ?;", ["types", 1])
    row = cur.fetchone()
    assert row == {"pk": "types", "sk": Decimal(1), **{k: _expect(v) for k, v in item.items()}}
    assert cur.fetchone() is None


def _expect(v: object) -> object:
    # What a bound value reads back as (N comes back as Decimal).
    if isinstance(v, bool) or v is None or isinstance(v, (str, bytes)):
        return v
    if isinstance(v, (int, float)):
        return Decimal(str(v))
    if isinstance(v, list):
        return [_expect(e) for e in v]
    if isinstance(v, dict):
        return {k: _expect(e) for k, e in v.items()}
    if isinstance(v, set):
        return {_expect(e) for e in v}
    return v


def test_query_bulk_update_and_ccf(conn: ddbsh.Connection) -> None:
    cur = conn.cursor()
    cur.executemany(
        f"insert into {TABLE} ( pk, sk, v, l ) values ( ?, ?, ?, ? );",
        [["q", i, i * 10, [i, None] if i % 2 else [i]] for i in range(1, 21)],
    )
    cur.execute(f"explain select * from {TABLE} where pk = ? and sk > ?;", ["q", 5])
    assert cur.kind == "explain"
    assert cur.explain.startswith("Query(")
    cur.execute(f"select sk from {TABLE} where pk = ? and sk > ?;", ["q", 5])
    assert [r["sk"] for r in cur] == [Decimal(i) for i in range(6, 21)]
    cur.execute(f"select sk from {TABLE} where pk = ? and contains(l, ?);", ["q", None])
    assert sorted(r["sk"] for r in cur.fetchall()) == [Decimal(i) for i in range(1, 21, 2)]
    cur.execute(f"update {TABLE} set w = ? where pk = ? and v > ?;", [True, "q", 150])
    assert cur.write_result == ddbsh.WriteResult("UPDATE", 5, 5, 0)
    with pytest.raises(ddbsh.IntegrityError) as info:
        cur.execute(f"insert into {TABLE} ( pk, sk ) values ( ?, ? );", ["q", 1])
    assert str(info.value) == "The conditional request failed"


def test_transaction(conn: ddbsh.Connection) -> None:
    cur = conn.cursor()
    cur.execute(
        f"begin; insert into {TABLE} ( pk, sk, v ) values ( ?, ?, ? ); "
        f"update {TABLE} set v = ? where pk = ? and sk = ?; commit;",
        ["txn", 1, 1, 2, "types", 1],
    )
    assert cur.status == "COMMIT"
    cur.execute(f'select v from {TABLE} where pk = "types" and sk = 1;')
    assert cur.fetchone() == {"v": Decimal(2)}


def test_errors(conn: ddbsh.Connection) -> None:
    cur = conn.cursor()
    with pytest.raises(ddbsh.ProgrammingError) as info:
        cur.execute("select * from ddbsh_py_live_nosuch;")
    assert info.value.service_error == "ResourceNotFoundException"
    assert "ddbsh_py_live_nosuch not found" in str(info.value)


def test_run_formats(conn: ddbsh.Connection) -> None:
    q = f'select pk, sk, v from {TABLE} where pk = "txn" and sk = 1;'
    assert conn.run(q) == "{pk: txn, sk: 1, v: 1}\n"
    assert conn.run(q, format="json", json_flavor="dynamodb") == (
        '[\n  {"pk": {"S": "txn"}, "sk": {"N": "1"}, "v": {"N": "1"}}\n]\n'
    )


def test_cross_account_profile() -> None:
    """A profile with role_arn + source_profile: boto3 performs the AssumeRole."""
    session = boto3.Session(profile_name="cross-acct")
    account = session.client("sts").get_caller_identity()["Account"]
    with ddbsh.connect(session) as c:
        c.run(f"drop table if exists {CROSS_TABLE};")
        assert c.run(f"create table {CROSS_TABLE} ( pk number ) primary key ( pk hash );") == (
            "CREATE\n"
        )
        try:
            cur = c.cursor()
            cur.execute(f"insert into {CROSS_TABLE} ( pk ) values ( ? );", [7])
            cur.execute(f"select * from {CROSS_TABLE};")
            assert cur.fetchall() == [{"pk": Decimal(7)}]
        finally:
            assert c.run(f"drop table if exists {CROSS_TABLE};") == "DROP\n"
    assert account != boto3.Session().client("sts").get_caller_identity()["Account"]
