# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""Shared fixtures for the offline pytest suite (no AWS account needed)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Callable, Dict, List, Tuple

import pytest

import ddbsh

#: The golden results directory (tests/golden/results), for parity checks.
GOLDEN_RESULTS = Path(__file__).resolve().parents[2] / "tests" / "golden" / "results"


class FakeTransport:
    """A transport that answers from a per-operation handler and records every
    request. A handler takes the parsed request body and returns the response
    body (a str) or raises."""

    def __init__(self, handlers: Dict[str, Callable[[dict], str]] = None) -> None:
        self.handlers = dict(handlers or {})
        self.requests: List[Tuple[str, dict]] = []
        self.connects: List[Tuple[str, str]] = []
        self.region = "us-east-1"

    def send(self, operation: str, body: str) -> str:
        doc = json.loads(body)
        self.requests.append((operation, doc))
        handler = self.handlers.get(operation)
        if handler is None:
            return json.dumps(
                {
                    "__type": "com.amazon.coral.service#UnknownOperationException",
                    "message": f"fake transport: no handler for {operation}",
                }
            )
        return handler(doc)

    def on_connect(self, region: str, endpoint: str) -> None:
        self.connects.append((region, endpoint))

    def descriptor(self) -> str:
        return "fake transport"

    def operations(self) -> List[str]:
        return [op for op, _ in self.requests]


def describe_table(name: str, hash_key: str, range_key: str = None) -> str:
    """A DescribeTable response body for a table with the given key."""
    keys = [{"AttributeName": hash_key, "KeyType": "HASH"}]
    attrs = [{"AttributeName": hash_key, "AttributeType": "N"}]
    if range_key:
        keys.append({"AttributeName": range_key, "KeyType": "RANGE"})
        attrs.append({"AttributeName": range_key, "AttributeType": "N"})
    return json.dumps(
        {
            "Table": {
                "TableName": name,
                "KeySchema": keys,
                "AttributeDefinitions": attrs,
                "TableStatus": "ACTIVE",
                "ItemCount": 0,
                "TableSizeBytes": 0,
            }
        }
    )


@pytest.fixture
def mem() -> ddbsh.Connection:
    conn = ddbsh.connect_memory()
    yield conn
    conn.close()


@pytest.fixture
def movies(mem: ddbsh.Connection) -> ddbsh.Connection:
    """An in-memory connection with a `movies` table (id S hash, yr N range)."""
    out = mem.run(
        'create table movies ( id string, yr number ) primary key ( id hash, yr range ); '
        'insert into movies ( id, yr, title, genres ) values ( "m1", 2010, "Inception", ["a", "b"] ); '
        'insert into movies ( id, yr, title ) values ( "m2", 2020, "Tenet" );'
    )
    assert out == "CREATE\nINSERT\nINSERT\n", out
    return mem
