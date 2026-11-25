# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""The PEP 249 surface and the type mapping, over the in-memory connection."""

from __future__ import annotations

import threading
from decimal import Decimal

import pytest

import ddbsh


def test_module_globals() -> None:
    assert ddbsh.apilevel == "2.0"
    assert ddbsh.threadsafety == 1
    assert ddbsh.paramstyle == "qmark"
    assert ddbsh.__version__.count(".") == 2
    for name in (
        "Warning",
        "Error",
        "InterfaceError",
        "DatabaseError",
        "DataError",
        "OperationalError",
        "IntegrityError",
        "InternalError",
        "ProgrammingError",
        "NotSupportedError",
    ):
        assert issubclass(getattr(ddbsh, name), Exception)
        assert getattr(ddbsh.Connection, name) is getattr(ddbsh, name)
    assert issubclass(ddbsh.ProgrammingError, ddbsh.DatabaseError)
    assert issubclass(ddbsh.DatabaseError, ddbsh.Error)
    assert issubclass(ddbsh.InterfaceError, ddbsh.Error)


def test_select_with_params(movies: ddbsh.Connection) -> None:
    cur = movies.cursor()
    assert cur.kind is None
    assert cur.execute("select * from movies where id = ? and yr = ?;", ["m1", 2010]) is cur
    assert cur.kind == "rows"
    assert cur.description == ()
    row = cur.fetchone()
    assert row == {"id": "m1", "yr": Decimal("2010"), "title": "Inception", "genres": ["a", "b"]}
    assert [d[0] for d in cur.description] == ["id", "yr", "title", "genres"]
    assert all(len(d) == 7 for d in cur.description)
    assert cur.fetchone() is None
    assert cur.rowcount == -1


def test_fetchmany_fetchall_iteration(movies: ddbsh.Connection) -> None:
    cur = movies.cursor()
    cur.execute("select id from movies;")
    assert len(cur.fetchmany()) == 1  # arraysize 1
    assert len(cur.fetchmany(5)) == 1
    assert cur.fetchmany(5) == []
    cur.execute("select id from movies;")
    assert sorted(r["id"] for r in cur.fetchall()) == ["m1", "m2"]
    cur.execute("select id from movies;")
    assert sorted(r["id"] for r in cur) == ["m1", "m2"]


def test_type_round_trip(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.execute("create table t ( id string ) primary key ( id hash );")
    assert cur.kind == "status" and cur.status == "CREATE"
    values = {
        "s": "hello, world",
        "n": Decimal("3.14159265358979323846264338327950288"),
        "i": 42,
        "neg": -7,
        "f": 2.5,
        "b": b"\x00\x01\xff",
        "ba": bytearray(b"xy"),
        "t": True,
        "nul": None,
        "l": [1, "x", None, False, [2]],
        "m": {"k": {"deep": [Decimal("1.5")]}, "e": {}},
        "ss": {"a", "b"},
        "ns": {1, Decimal("2.5")},
        "bs": {b"p", b"q"},
        "el": [],
    }
    names = list(values)
    cur.execute(
        f"insert into t ( id, {', '.join(names)} ) values ( ?, {', '.join('?' for _ in names)} );",
        ["k1", *values.values()],
    )
    assert cur.status == "INSERT"
    cur.execute('select * from t where id = "k1";')
    row = cur.fetchone()
    assert row["s"] == "hello, world"
    assert row["n"] == Decimal("3.14159265358979323846264338327950288")
    assert row["i"] == Decimal(42) and isinstance(row["i"], Decimal)
    assert row["neg"] == Decimal(-7)
    assert row["f"] == Decimal("2.5")
    assert row["b"] == b"\x00\x01\xff" and isinstance(row["b"], bytes)
    assert row["ba"] == b"xy"
    assert row["t"] is True
    assert row["nul"] is None and "nul" in row
    assert row["l"] == [Decimal(1), "x", None, False, [Decimal(2)]]
    assert row["m"] == {"k": {"deep": [Decimal("1.5")]}, "e": {}}
    assert row["ss"] == {"a", "b"}
    assert row["ns"] == {Decimal(1), Decimal("2.5")}
    assert row["bs"] == {b"p", b"q"}
    assert row["el"] == []


@pytest.mark.parametrize(
    "value, message",
    [
        (set(), "empty set"),
        ({"a", 1}, "all str, all numbers or all bytes"),
        ({1: "x"}, "map keys must be str"),
        (float("nan"), "non-finite float"),
        (float("inf"), "non-finite float"),
        (Decimal("NaN"), "non-finite Decimal"),
        (object(), "cannot bind a value of type object"),
    ],
)
def test_unbindable_values(mem: ddbsh.Connection, value: object, message: str) -> None:
    cur = mem.cursor()
    cur.execute("create table t ( id string ) primary key ( id hash );")
    with pytest.raises(ddbsh.ProgrammingError, match=message) as info:
        cur.execute("insert into t ( id, v ) values ( ?, ? );", ["k", value])
    assert info.value.code == "invalid_argument"


def test_parameter_count_and_kind(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.execute("create table t ( id string ) primary key ( id hash );")
    with pytest.raises(ddbsh.ProgrammingError, match="1 parameter marker but 2 parameters"):
        cur.execute("select * from t where id = ?;", ["a", "b"])
    with pytest.raises(ddbsh.ProgrammingError, match="2 parameter markers but 1 parameter was"):
        cur.execute("insert into t ( id, v ) values ( ?, ? );", ["a"])
    with pytest.raises(ddbsh.ProgrammingError, match="paramstyle is qmark"):
        cur.execute("select * from t where id = ?;", {"id": "a"})
    with pytest.raises(ddbsh.ProgrammingError, match="paramstyle is qmark"):
        cur.execute("select * from t where id = ?;", "a")


def test_unbound_marker_without_params(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.execute("create table t ( id string ) primary key ( id hash );")
    with pytest.raises(ddbsh.ProgrammingError) as info:
        cur.execute("select * from t where id = ?;")
    assert info.value.code == "unbound_parameter"


def test_prepared_statement_reuse(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.execute("create table t ( id string ) primary key ( id hash );")
    cur.executemany("insert into t ( id, v ) values ( ?, ? );", [["a", 1], ["b", 2], ["c", 3]])
    cur.execute("select id, v from t where v > ?;", [1])
    assert sorted(r["id"] for r in cur.fetchall()) == ["b", "c"]


def test_contains_parameter(mem: ddbsh.Connection) -> None:
    """D-P13-13: `?` is a CONTAINS operand; NULL, a number, a bool and a
    string each bind."""
    cur = mem.cursor()
    cur.execute("create table t ( id string ) primary key ( id hash );")
    cur.execute("insert into t ( id, l ) values ( ?, ? );", ["a", [1, None, "x"]])
    cur.execute("insert into t ( id, l ) values ( ?, ? );", ["b", [True, 2]])
    for operand, expected in ((None, ["a"]), (2, ["b"]), (True, ["b"]), ("x", ["a"]), (9, [])):
        cur.execute("select id from t where contains(l, ?);", [operand])
        assert sorted(r["id"] for r in cur.fetchall()) == expected, operand


def test_bulk_write_counts(movies: ddbsh.Connection) -> None:
    cur = movies.cursor()
    cur.execute("update movies set rating = ? where yr > ?;", [5, 2000])
    assert cur.kind == "write"
    assert cur.write_result == ddbsh.WriteResult("UPDATE", 2, 2, 0)
    assert cur.rowcount == 2
    assert cur.description is None
    with pytest.raises(ddbsh.ProgrammingError, match="returns no rows"):
        cur.fetchone()


def test_conditional_check_is_integrity_error(movies: ddbsh.Connection) -> None:
    cur = movies.cursor()
    with pytest.raises(ddbsh.IntegrityError) as info:
        cur.execute('insert into movies ( id, yr ) values ( "m1", 2010 );')
    assert str(info.value) == "The conditional request failed"
    assert info.value.code == "conditional_check_failed"


def test_errors_carry_cli_text(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    with pytest.raises(ddbsh.ProgrammingError) as info:
        cur.execute("select * from nosuch;")
    assert str(info.value) == "Table: nosuch not found"
    assert info.value.code == "no_such_table"
    with pytest.raises(ddbsh.ProgrammingError) as info:
        cur.execute("selec;")
    assert str(info.value) == "[1, 6] syntax error at selec\nselec;\n^"
    assert info.value.code == "parse_error"


def test_batch_result_sets_and_late_error(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.execute(
        "create table t ( id string ) primary key ( id hash ); "
        'insert into t ( id ) values ( "a" ); '
        "select * from t; "
        "select * from nosuch; "
        "show version;"
    )
    assert cur.status == "CREATE"
    assert cur.nextset() is True and cur.status == "INSERT"
    assert cur.nextset() is True and cur.fetchall() == [{"id": "a"}]
    with pytest.raises(ddbsh.ProgrammingError, match="Table: nosuch not found"):
        cur.nextset()
    assert cur.nextset() is None


def test_report_and_render(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.execute("show version;")
    assert cur.kind == "report"
    assert cur.report == [f"ddbsh - version {ddbsh.__version__}"]
    assert cur.render() == f"ddbsh - version {ddbsh.__version__}\n"


def test_explain_not_supported_in_memory(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.execute("create table t ( id string ) primary key ( id hash );")
    with pytest.raises(ddbsh.NotSupportedError):
        cur.execute('explain select * from t where id = "a";')


def test_closed_connection_and_cursor(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.close()
    cur.close()  # idempotent
    with pytest.raises(ddbsh.InterfaceError):
        cur.execute("show version;")
    other = mem.cursor()
    mem.commit()
    mem.rollback()
    mem.close()
    assert mem.closed
    for call in (
        lambda: other.execute("show version;"),
        lambda: mem.cursor(),
        lambda: mem.run("show version;"),
        lambda: mem.commit(),
    ):
        with pytest.raises(ddbsh.InterfaceError, match="connection is closed"):
            call()


def test_context_managers() -> None:
    with ddbsh.connect_memory() as conn:
        with conn.cursor() as cur:
            cur.execute("show version;")
        with pytest.raises(ddbsh.InterfaceError):
            cur.execute("show version;")
    assert conn.closed


def test_cursors_are_independent(movies: ddbsh.Connection) -> None:
    a = movies.cursor()
    b = movies.cursor()
    a.execute("select id from movies;")
    first = a.fetchone()
    b.execute('select * from movies where id = "m2" and yr = 2020;')
    assert b.fetchone()["title"] == "Tenet"
    assert a.fetchone()["id"] != first["id"]


def test_threads_share_a_connection(movies: ddbsh.Connection) -> None:
    """Two threads using one connection take turns; nothing crashes."""
    errors = []

    def work() -> None:
        try:
            cur = movies.cursor()
            for _ in range(50):
                cur.execute("select id from movies;")
                assert len(cur.fetchall()) == 2
        except Exception as e:  # pragma: no cover - reported below
            errors.append(e)

    threads = [threading.Thread(target=work) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []


def test_threads_with_own_connections() -> None:
    errors = []

    def work(i: int) -> None:
        try:
            conn = ddbsh.connect_memory()
            assert conn.run(f"create table t{i} ( id string ) primary key ( id hash );") == "CREATE\n"
        except Exception as e:  # pragma: no cover
            errors.append(e)

    threads = [threading.Thread(target=work, args=(i,)) for i in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert errors == []
