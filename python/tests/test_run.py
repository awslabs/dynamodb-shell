# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""Connection.run: the command's exact text, formats, statuses, timely output."""

from __future__ import annotations

import pytest

import ddbsh


def test_run_text_and_segments(mem: ddbsh.Connection) -> None:
    result = mem.run_result(
        "create table t ( pk number ) primary key ( pk hash ); select * from nosuch; drop table t;"
    )
    assert result.segments == [
        ("stdout", "CREATE\n"),
        ("stderr", "Table: nosuch not found\n"),
        ("stdout", "DROP\n"),
    ]
    assert result.output == "CREATE\nTable: nosuch not found\nDROP\n"
    # The exit status is the last statement's, as in the command.
    assert result.status == 0
    assert mem.run_result("select * from nosuch;").status == 1


def test_run_formats(movies: ddbsh.Connection) -> None:
    q = 'select * from movies where id = "m2" and yr = 2020;'
    assert movies.run(q) == "{id: m2, title: Tenet, yr: 2020}\n"
    assert movies.run(q, format="json") == '[\n  {"id": "m2", "title": "Tenet", "yr": 2020}\n]\n'
    assert movies.run(q, format="json", json_flavor="dynamodb") == (
        '[\n  {"id": {"S": "m2"}, "title": {"S": "Tenet"}, "yr": {"N": "2020"}}\n]\n'
    )
    # CSV columns are in first-seen order.
    assert movies.run(q, format="csv") == "id,yr,title\nm2,2020,Tenet\n"
    # `set output` in the text changes the format, as in the command.
    assert movies.run("set output csv\n" + q) == "id,yr,title\nm2,2020,Tenet\n"


def test_run_bad_format(mem: ddbsh.Connection) -> None:
    with pytest.raises(ValueError, match=r"unknown output format: xml \(expected"):
        mem.run("show version;", format="xml")
    with pytest.raises(ValueError, match=r"unknown JSON flavor: typed \(expected"):
        mem.run("show version;", json_flavor="typed")


def test_run_abort_on_error(mem: ddbsh.Connection) -> None:
    text = "select * from nosuch; show version;"
    assert "version" in mem.run(text)
    result = mem.run_result(text, abort_on_error=True)
    assert result.output == "Table: nosuch not found\n"
    assert result.status == 1


def test_run_on_output_is_timely_and_ordered(mem: ddbsh.Connection) -> None:
    seen = []
    result = mem.run_result(
        "create table t ( pk number ) primary key ( pk hash ); select * from nosuch; show version;",
        on_output=lambda stream, text: seen.append((stream, text)),
    )
    assert "".join(t for _, t in seen) == result.output
    assert [s for s, _ in seen] == ["stdout", "stderr", "stdout"]


def test_run_on_output_exception_is_raised_after(mem: ddbsh.Connection) -> None:
    calls = []

    def boom(stream: str, text: str) -> None:
        calls.append(text)
        raise RuntimeError("stop")

    with pytest.raises(RuntimeError, match="stop"):
        mem.run("show version; show version;", on_output=boom)
    assert len(calls) == 1  # no calls after the first exception


def test_run_on_output_cannot_reenter(mem: ddbsh.Connection) -> None:
    def reenter(stream: str, text: str) -> None:
        mem.run("show version;")

    with pytest.raises(ddbsh.ProgrammingError, match="from inside its own"):
        mem.run("show version;", on_output=reenter)
    # The connection still works afterwards.
    assert mem.run("show version;").startswith("ddbsh - version")


def test_help_text(mem: ddbsh.Connection) -> None:
    assert "SELECT" in mem.run("help select;")


def test_region_and_connect(mem: ddbsh.Connection) -> None:
    assert mem.region == "us-east-1"
    assert mem.endpoint_overridden is False
    assert mem.run('connect "us-west-2";') == "CONNECT\n"
    assert mem.region == "us-west-2"
