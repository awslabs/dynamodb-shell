# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""Stopping a run (Ctrl-C, a failing output callback) and object lifetime:
a KeyboardInterrupt is never lost and stops the remaining DynamoDB calls, and
reference cycles through a connection or a cursor are collectable."""

from __future__ import annotations

import gc
import json
import subprocess
import sys
import textwrap
import weakref

import pytest

import ddbsh

from conftest import FakeTransport, describe_table


def _deleted(name: str) -> str:
    return json.dumps({"TableDescription": {"TableName": name, "TableStatus": "DELETING"}})


def _drop_transport(first: BaseException) -> FakeTransport:
    """DeleteTable raises `first` on the first call and RuntimeError after."""
    calls = {"n": 0}

    def delete(doc: dict) -> str:
        calls["n"] += 1
        if calls["n"] == 1:
            raise first
        raise RuntimeError("later failure")

    return FakeTransport({"DeleteTable": delete})


DROP_THREE = "drop table nowait a; drop table nowait b; drop table nowait c;"


def test_run_interrupt_in_transport_stops_the_batch() -> None:
    t = _drop_transport(KeyboardInterrupt())
    conn = ddbsh.connect(transport=t)
    with pytest.raises(KeyboardInterrupt):
        conn.run(DROP_THREE)
    assert t.operations() == ["DeleteTable"]
    # The transport sends again after the interrupt was raised.
    t.handlers["DeleteTable"] = lambda doc: _deleted(doc["TableName"])
    assert conn.run("drop table nowait d;") == "DROP\n"


def test_run_system_exit_in_transport_stops_the_batch() -> None:
    t = _drop_transport(SystemExit(3))
    conn = ddbsh.connect(transport=t)
    with pytest.raises(SystemExit):
        conn.run(DROP_THREE)
    assert t.operations() == ["DeleteTable"]


def test_run_interrupt_output_is_not_delivered() -> None:
    t = _drop_transport(KeyboardInterrupt())
    seen = []
    conn = ddbsh.connect(transport=t)
    with pytest.raises(KeyboardInterrupt):
        conn.run(DROP_THREE, on_output=lambda stream, text: seen.append(text))
    assert seen == []  # no "not sent" lines for the drained statements


def test_run_interrupt_in_on_output_stops_the_batch() -> None:
    t = FakeTransport({"DeleteTable": lambda doc: _deleted(doc["TableName"])})
    conn = ddbsh.connect(transport=t)

    def interrupt(stream: str, text: str) -> None:
        raise KeyboardInterrupt

    with pytest.raises(KeyboardInterrupt):
        conn.run(DROP_THREE, on_output=interrupt)
    assert t.operations() == ["DeleteTable"]


def test_run_on_output_exception_stops_the_batch() -> None:
    t = FakeTransport({"DeleteTable": lambda doc: _deleted(doc["TableName"])})
    conn = ddbsh.connect(transport=t)
    calls = []

    def boom(stream: str, text: str) -> None:
        calls.append(text)
        raise RuntimeError("stop")

    with pytest.raises(RuntimeError, match="stop"):
        conn.run(DROP_THREE, on_output=boom)
    assert t.operations() == ["DeleteTable"]
    assert calls == ["DROP\n"]
    # The next run sends again.
    assert conn.run("drop table nowait d;") == "DROP\n"


def test_retry_callback_interrupt_stops_the_retry() -> None:
    throttled = json.dumps(
        {"__type": "com.amazonaws.dynamodb.v20120810#ThrottlingException", "message": "slow"}
    )
    t = FakeTransport({"DescribeTable": lambda doc: throttled})

    def interrupt(event: object) -> None:
        raise KeyboardInterrupt

    conn = ddbsh.connect(transport=t, retry_callback=interrupt)
    with pytest.raises(KeyboardInterrupt):
        conn.run("explain select * from isel1; explain select * from isel1;")
    assert t.operations() == ["DescribeTable"]  # no retry was sent
    with pytest.raises(KeyboardInterrupt):
        conn.cursor().execute("select * from isel1;")
    conn.set_retry_callback(None)


def test_cursor_interrupt_raised_at_nextset() -> None:
    calls = {"n": 0}

    def delete(doc: dict) -> str:
        calls["n"] += 1
        if calls["n"] == 2:
            raise KeyboardInterrupt
        return _deleted(doc["TableName"])

    t = FakeTransport({"DeleteTable": delete})
    cur = ddbsh.connect(transport=t).cursor()
    cur.execute(DROP_THREE)
    assert cur.status == "DROP"
    with pytest.raises(KeyboardInterrupt):
        cur.nextset()
    assert t.operations() == ["DeleteTable", "DeleteTable"]


def test_unbindable_value_leaves_no_result_set(mem: ddbsh.Connection) -> None:
    cur = mem.cursor()
    cur.execute("show version;")
    assert cur.kind == "report"
    with pytest.raises(ddbsh.ProgrammingError, match="cannot bind"):
        cur.execute("select * from t where pk = ?;", [object()])
    assert cur.kind is None


# ---- reference cycles ----------------------------------------------------------


class _Holder(FakeTransport):
    """A transport that can hold references back into its connection."""


def _collected(*refs: weakref.ref) -> bool:
    gc.collect()
    return all(r() is None for r in refs)


def test_cycle_through_transport_is_collected() -> None:
    t = _Holder({"DescribeTable": lambda doc: describe_table("isel1", "a", "b")})
    conn = ddbsh.connect(transport=t)
    t.conn = conn
    refs = (weakref.ref(conn), weakref.ref(t))
    del conn, t
    assert _collected(*refs)


def test_cycle_through_retry_callback_is_collected() -> None:
    t = FakeTransport()
    conn = ddbsh.connect(transport=t)
    conn.set_retry_callback(lambda event: conn.region)
    refs = (weakref.ref(conn), weakref.ref(t))
    del conn, t
    assert _collected(*refs)


def test_cycle_through_cursor_is_collected() -> None:
    t = _Holder({"DescribeTable": lambda doc: describe_table("isel1", "a", "b")})
    conn = ddbsh.connect(transport=t)
    t.cur = conn.cursor()
    t.cur.execute("explain select * from isel1;")
    refs = (weakref.ref(conn), weakref.ref(t), weakref.ref(t.cur))
    del conn, t
    assert _collected(*refs)


def test_cursor_outlives_its_connection_object() -> None:
    conn = ddbsh.connect_memory()
    cur = conn.cursor()
    del conn
    gc.collect()
    cur.execute("show version;")
    assert cur.kind == "report"


def test_no_leak_warnings_at_exit() -> None:
    """A module-level script that keeps a connection, a cursor and a caught
    transport error alive until interpreter exit prints nothing on stderr."""
    script = textwrap.dedent(
        """
        import ddbsh

        class T:
            def send(self, op, body):
                raise ConnectionError("down")
            def on_connect(self, region, endpoint):
                pass
            def descriptor(self):
                return "t"

        conn = ddbsh.connect(transport=T())
        cur = conn.cursor()
        try:
            cur.execute("select * from t;")
        except Exception as exc:
            kept = exc
        t = T()
        t.cur = ddbsh.connect(transport=t).cursor()
        """
    )
    proc = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True, timeout=60
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stderr == ""


def test_close_closes_the_transport() -> None:
    class Closing(FakeTransport):
        closed = 0

        def close(self) -> None:
            self.closed += 1

    t = Closing()
    conn = ddbsh.connect(transport=t)
    conn.close()
    conn.close()
    assert t.closed == 1
