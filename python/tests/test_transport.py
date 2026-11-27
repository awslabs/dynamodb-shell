# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""The wire path offline: a fake transport behind ddbsh.connect(transport=...),
EXPLAIN parity with the golden files, retries, transport failures, and the
botocore transport's signing (no network)."""

from __future__ import annotations

import json
import re
import sys

import botocore.session
import pytest
from botocore.awsrequest import AWSResponse

import ddbsh
from ddbsh.transport import (
    BotocoreTransport,
    TransportError,
    mask_access_key_id,
    search_endpoint,
)

from conftest import GOLDEN_RESULTS, FakeTransport, describe_table


def isel1() -> FakeTransport:
    return FakeTransport({"DescribeTable": lambda doc: describe_table(doc["TableName"], "a", "b")})


def _isel1_explain_goldens() -> list:
    """Every select-family golden that EXPLAINs a SELECT on isel1 (a HASH,
    b RANGE): (name, sql). The planner needs only the key schema, which the
    fake transport's DescribeTable supplies, so these run offline."""
    family = (GOLDEN_RESULTS.parent / "families" / "select.txt").read_text()
    pattern = re.compile(
        r'add_test\(NAME (isel-\d+) COMMAND ddbshtest -T \$\{RD\}/\1\.exp -c '
        r'"(explain select (?:[^"\\]|\\.)* from isel1(?: (?:[^"\\]|\\.)*)?)"\)'
    )
    return [(m.group(1), m.group(2).replace('\\"', '"')) for m in pattern.finditer(family)]


ISEL1_EXPLAIN = _isel1_explain_goldens()


def test_explain_goldens_found() -> None:
    assert len(ISEL1_EXPLAIN) >= 50


@pytest.mark.parametrize("golden, sql", ISEL1_EXPLAIN, ids=[g for g, _ in ISEL1_EXPLAIN])
def test_explain_matches_golden(golden: str, sql: str) -> None:
    expected = (GOLDEN_RESULTS / f"{golden}.exp").read_text()
    conn = ddbsh.connect(transport=isel1())
    assert conn.run(sql) == expected


def test_query_rows_and_request(movies_wire: tuple) -> None:
    conn, fake = movies_wire
    cur = conn.cursor()
    cur.execute("select * from isel1 where a = ? and b > ?;", [2, 3])
    assert cur.fetchall() == [{"a": 2, "b": 4, "c": "x"}]
    op, doc = fake.requests[-1]
    assert op == "Query"
    assert doc["KeyConditionExpression"] == "#aaaa1 = :vaaa1 AND #aaaa2 > :vaaa2"
    assert doc["ExpressionAttributeValues"] == {":vaaa1": {"N": "2"}, ":vaaa2": {"N": "3"}}


def test_not_equal_on_sort_key_sends_two_queries() -> None:
    """P15, D-P15-8: `b <> v` with the hash key `=` reads `b < v`, then
    `b > v`, through the wire path, and the rows come back in that order."""

    def query(doc: dict) -> str:
        below = " < " in doc["KeyConditionExpression"]
        b = "1" if below else "7"
        items = [{"a": {"N": "2"}, "b": {"N": b}}]
        return json.dumps({"Items": items, "Count": 1, "ScannedCount": 1})

    for op in ("<>", "!="):
        fake = isel1()
        fake.handlers["Query"] = query
        conn = ddbsh.connect(transport=fake)
        cur = conn.cursor()
        cur.execute(f"select * from isel1 where a = ? and b {op} ?;", [2, 5])
        assert cur.fetchall() == [{"a": 2, "b": 1}, {"a": 2, "b": 7}], op
        queries = [doc for name, doc in fake.requests if name == "Query"]
        assert [d["KeyConditionExpression"] for d in queries] == [
            "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2",
            "#aaaa1 = :vaaa1 AND #aaaa2 > :vaaa2",
        ], op
        values = {":vaaa1": {"N": "2"}, ":vaaa2": {"N": "5"}}
        assert all(d["ExpressionAttributeValues"] == values for d in queries)
        assert all("ExclusiveStartKey" not in d for d in queries)
        conn.close()


@pytest.fixture
def movies_wire() -> tuple:
    fake = isel1()
    fake.handlers["Query"] = lambda doc: json.dumps(
        {
            "Items": [{"a": {"N": "2"}, "b": {"N": "4"}, "c": {"S": "x"}}],
            "Count": 1,
            "ScannedCount": 1,
        }
    )
    return ddbsh.connect(transport=fake), fake


def test_service_error_text() -> None:
    fake = FakeTransport(
        {
            "DescribeTable": lambda doc: json.dumps(
                {
                    "__type": "com.amazonaws.dynamodb.v20120810#ResourceNotFoundException",
                    "message": "Requested resource not found: Table: nosuch not found",
                }
            )
        }
    )
    conn = ddbsh.connect(transport=fake)
    assert conn.run("select * from nosuch;") == (
        "Requested resource not found: Table: nosuch not found\n"
    )
    with pytest.raises(ddbsh.ProgrammingError) as info:
        conn.cursor().execute("select * from nosuch;")
    assert str(info.value) == "Requested resource not found: Table: nosuch not found"
    assert info.value.service_error == "ResourceNotFoundException"


def test_retries_raise_events() -> None:
    attempts = {"n": 0}

    def describe(doc: dict) -> str:
        attempts["n"] += 1
        if attempts["n"] < 3:
            return json.dumps(
                {
                    "__type": "com.amazonaws.dynamodb.v20120810#ThrottlingException",
                    "message": "Rate exceeded",
                }
            )
        return describe_table("isel1", "a", "b")

    events = []
    conn = ddbsh.connect(
        transport=FakeTransport({"DescribeTable": describe}), retry_callback=events.append
    )
    assert conn.run("explain select * from isel1").startswith("Scan(")
    assert attempts["n"] == 3
    assert [(e.operation, e.attempt, e.throttled) for e in events] == [
        ("DescribeTable", 2, True),
        ("DescribeTable", 3, True),
    ]
    assert events[0].error_code == "ThrottlingException"
    assert events[0].max_attempts == 11
    assert events[0].delay_ms is not None and events[0].delay_ms >= 0
    conn.set_retry_callback(None)


def test_retry_callback_exception_does_not_stop_retry(monkeypatch: pytest.MonkeyPatch) -> None:
    reported = []
    monkeypatch.setattr(sys, "unraisablehook", reported.append)
    attempts = {"n": 0}

    def describe(doc: dict) -> str:
        attempts["n"] += 1
        if attempts["n"] == 1:
            return json.dumps({"__type": "x#ThrottlingException", "message": "slow down"})
        return describe_table("isel1", "a", "b")

    def bad(event: ddbsh.RetryEvent) -> None:
        raise RuntimeError("callback failed")

    conn = ddbsh.connect(transport=FakeTransport({"DescribeTable": describe}), retry_callback=bad)
    assert conn.run("explain select * from isel1").startswith("Scan(")
    assert attempts["n"] == 2
    # The exception went to sys.unraisablehook, and the retry still happened.
    assert len(reported) == 1
    assert isinstance(reported[0].exc_value, RuntimeError)
    reported.clear()  # the hook arguments hold the traceback, and so the event


def test_transport_exception_is_operational_error_with_cause() -> None:
    def broken(doc: dict) -> str:
        raise ConnectionError("network is down")

    conn = ddbsh.connect(transport=FakeTransport({"DescribeTable": broken}))
    with pytest.raises(ddbsh.OperationalError) as info:
        conn.cursor().execute("select * from isel1;")
    assert "wire transport: ConnectionError: network is down" in str(info.value)
    assert isinstance(info.value.__cause__, ConnectionError)
    # run() reports it as text, as the command does.
    assert "network is down" in conn.run("select * from isel1;")


def test_transport_keyboard_interrupt_propagates() -> None:
    def interrupt(doc: dict) -> str:
        raise KeyboardInterrupt

    conn = ddbsh.connect(transport=FakeTransport({"DescribeTable": interrupt}))
    with pytest.raises(KeyboardInterrupt):
        conn.run("select * from isel1;")
    with pytest.raises(KeyboardInterrupt):
        conn.cursor().execute("select * from isel1;")


def test_transport_must_return_str() -> None:
    conn = ddbsh.connect(transport=FakeTransport({"DescribeTable": lambda doc: b"{}"}))
    with pytest.raises(ddbsh.OperationalError, match="send\\(\\) returned bytes, expected str"):
        conn.cursor().execute("select * from isel1;")


def test_transport_cannot_reenter() -> None:
    holder = {}

    def reenter(doc: dict) -> str:
        holder["conn"].run("show version;")
        return describe_table("isel1", "a", "b")

    conn = ddbsh.connect(transport=FakeTransport({"DescribeTable": reenter}))
    holder["conn"] = conn
    with pytest.raises(ddbsh.OperationalError) as info:
        conn.cursor().execute("select * from isel1;")
    # The re-entrant run() raised ProgrammingError inside the transport.
    assert isinstance(info.value.__cause__, ddbsh.ProgrammingError)
    assert "from inside its own transport" in str(info.value.__cause__)


def test_connect_statement_reaches_transport() -> None:
    fake = FakeTransport()
    conn = ddbsh.connect(transport=fake)
    assert conn.run('connect "us-west-2" with endpoint "http://localhost:8000";') == "CONNECT\n"
    assert fake.connects == [("us-west-2", "http://localhost:8000")]
    assert conn.region == "us-west-2"
    assert conn.endpoint_overridden
    assert "Backend: fake transport" in conn.run("show connection;")


# ---- BotocoreTransport, without a network -------------------------------------


class _Http:
    """Stands in for botocore's URLLib3Session: records and answers."""

    def __init__(self, status: int, body: str) -> None:
        self.status = status
        self.body = body
        self.sent = []

    def send(self, request):
        self.sent.append(request)
        return AWSResponse(request.url, self.status, {}, _Raw(self.body.encode()))

    def close(self) -> None:
        pass


class _Raw:
    def __init__(self, data: bytes) -> None:
        self.data = data

    def stream(self, *args, **kwargs):
        yield self.data


@pytest.fixture
def session() -> botocore.session.Session:
    s = botocore.session.Session()
    s.set_credentials("AKIDEXAMPLEKEY12345", "secret", "token")
    s.set_config_variable("region", "eu-west-1")
    return s


def test_botocore_transport_signs_and_targets(session: botocore.session.Session) -> None:
    t = BotocoreTransport(session)
    assert t.region == "eu-west-1"
    assert t.endpoint == "https://dynamodb.eu-west-1.amazonaws.com"
    http = _Http(200, '{"TableNames": []}')
    t._http = t._search_http = http
    assert t.send("ListTables", '{"Limit":1}') == '{"TableNames": []}'
    req = http.sent[0]
    assert req.method == "POST"
    assert req.url.rstrip("/") == "https://dynamodb.eu-west-1.amazonaws.com"
    assert req.headers["X-Amz-Target"] == "DynamoDB_20120810.ListTables"
    assert req.headers["Content-Type"] == "application/x-amz-json-1.0"
    auth = req.headers["Authorization"]
    auth = auth.decode() if isinstance(auth, bytes) else auth
    assert auth.startswith("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLEKEY12345/")
    assert "/eu-west-1/dynamodb/aws4_request" in auth
    token = req.headers["X-Amz-Security-Token"]
    assert (token.decode() if isinstance(token, bytes) else token) == "token"
    assert req.body == b'{"Limit":1}'


def test_botocore_transport_error_bodies(session: botocore.session.Session) -> None:
    t = BotocoreTransport(session)
    error = '{"__type":"x#ValidationException","message":"bad"}'
    t._http = t._search_http = _Http(400, error)
    assert t.send("Query", "{}") == error
    t._http = t._search_http = _Http(502, "<html>bad gateway</html>")
    with pytest.raises(TransportError, match="HTTP 502"):
        t.send("Query", "{}")


def test_botocore_transport_retarget_and_descriptor(session: botocore.session.Session) -> None:
    t = BotocoreTransport(session, region="us-east-2")
    assert t.descriptor().endswith(
        "-> https://dynamodb.us-east-2.amazonaws.com (default); credentials AKID…2345"
    )
    t.on_connect("us-west-1", "localhost:8000")
    assert (t.region, t.endpoint, t.endpoint_overridden) == (
        "us-west-1",
        "https://localhost:8000",
        True,
    )
    assert "-> https://localhost:8000; credentials" in t.descriptor()
    t.on_connect("us-west-1", "")
    assert t.endpoint == "https://dynamodb.us-west-1.amazonaws.com"


def test_botocore_transport_no_credentials() -> None:
    s = botocore.session.Session()
    s.set_config_variable("region", "us-east-1")
    s.get_credentials = lambda: None
    t = BotocoreTransport(s)
    assert t.descriptor().endswith("credentials none")
    conn = ddbsh.connect(transport=t)
    with pytest.raises(ddbsh.OperationalError, match="Unable to locate credentials"):
        conn.cursor().execute("select * from t;")


def test_botocore_transport_verify_from_session(session: botocore.session.Session) -> None:
    session.set_config_variable("ca_bundle", "/tmp/ca.pem")
    assert BotocoreTransport(session)._verify == "/tmp/ca.pem"
    assert BotocoreTransport(session, verify=False)._verify is False


def test_search_vectors_goes_to_the_search_endpoint(session: botocore.session.Session) -> None:
    t = BotocoreTransport(session)
    assert t.search_endpoint == "https://search-dynamodb.eu-west-1.amazonaws.com"
    http = _Http(200, "{}")
    t._http = t._search_http = http
    t.send("SearchVectors", "{}")
    t.send("Query", "{}")
    assert [r.url.rstrip("/") for r in http.sent] == [
        "https://search-dynamodb.eu-west-1.amazonaws.com",
        "https://dynamodb.eu-west-1.amazonaws.com",
    ]
    # An override serves every operation.
    t.on_connect("eu-west-1", "http://localhost:8000")
    assert t.search_endpoint == "http://localhost:8000"


@pytest.mark.parametrize(
    "url, expected",
    [
        ("https://dynamodb.us-east-1.amazonaws.com", "https://search-dynamodb.us-east-1.amazonaws.com"),
        ("https://dynamodb-fips.us-gov-west-1.amazonaws.com", "https://search-dynamodb-fips.us-gov-west-1.amazonaws.com"),
        ("https://dynamodb.eu-west-1.api.aws", "https://search-dynamodb.eu-west-1.api.aws"),
        ("https://dynamodb.cn-north-1.amazonaws.com.cn", "https://search-dynamodb.cn-north-1.amazonaws.com.cn"),
        ("http://localhost:8000", "http://localhost:8000"),
        ("https://dynamodb.us-east-1.amazonaws.com:443", "https://dynamodb.us-east-1.amazonaws.com:443"),
        ("https://123456789012.ddb.us-east-1.amazonaws.com", "https://123456789012.ddb.us-east-1.amazonaws.com"),
    ],
)
def test_search_endpoint_mapping(url: str, expected: str) -> None:
    assert search_endpoint(url) == expected


def test_mask_access_key_id() -> None:
    assert mask_access_key_id("ASIAABCDEFGHWXYZ") == "ASIA…WXYZ"
    assert mask_access_key_id("short") == "…"


def test_connect_with_session(session: botocore.session.Session) -> None:
    conn = ddbsh.connect(session, endpoint_url="http://localhost:8000")
    assert conn.region == "eu-west-1"
    assert conn.endpoint_overridden
    assert conn.transport.endpoint == "http://localhost:8000"


def test_search_endpoint_proxy_follows_no_proxy(
    session: botocore.session.Session, monkeypatch: pytest.MonkeyPatch
) -> None:
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "NO_PROXY", "http_proxy", "https_proxy", "no_proxy"):
        monkeypatch.delenv(name, raising=False)
    t = BotocoreTransport(session)
    assert t._search_http is t._http  # same proxies: one connection pool
    monkeypatch.setenv("HTTPS_PROXY", "http://proxy.example:3128")
    monkeypatch.setenv("NO_PROXY", "search-dynamodb.eu-west-1.amazonaws.com")
    t = BotocoreTransport(session)
    assert t._search_http is not t._http
    t.close()
    assert t._http is None and t._search_http is None
    with pytest.raises(TransportError, match="closed"):
        t.send("Query", "{}")


def test_closed_transport_stays_closed(session: botocore.session.Session) -> None:
    # CONNECT on a closed transport does not build new connection pools.
    t = BotocoreTransport(session)
    t.close()
    with pytest.raises(TransportError, match="closed"):
        t.on_connect("us-west-2", "")
    assert t._http is None and t._search_http is None
    assert t.region == "eu-west-1"
    with pytest.raises(TransportError, match="closed"):
        t.send("Query", "{}")
