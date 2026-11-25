# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""ddbsh, the DynamoDB shell, as a Python library.

Run ddbsh's SQL-like statements against Amazon DynamoDB from Python, with the
same engine the ``ddbsh`` command uses, so the same statement makes the same
DynamoDB calls and prints the same text.

Two ways to use it:

* :meth:`Connection.run` returns the exact text the ``ddbsh`` command prints,
  in any output format::

      import ddbsh
      conn = ddbsh.connect()
      print(conn.run("select * from movies where id = 'm1';"))

* A PEP 249 (DB-API 2.0) cursor returns rows as dicts of Python values, with
  ``?`` parameters (``paramstyle = "qmark"``)::

      cur = conn.cursor()
      cur.execute("select * from movies where id = ? and yr = ?;", ["m1", 2010])
      for row in cur:
          print(row["title"])

Credentials, region, endpoint and CA trust come from boto3 (see
:func:`connect`). :func:`connect_memory` gives an in-memory connection for
tests and offline use. See docs/PYTHON.md for the full guide.
"""

from __future__ import annotations

from collections import namedtuple
from typing import Any, Callable, Iterator, List, Optional, Sequence, Tuple, Union

from . import _core
from ._errors import (
    DatabaseError,
    DataError,
    Error,
    IntegrityError,
    InterfaceError,
    InternalError,
    NotSupportedError,
    OperationalError,
    ProgrammingError,
    Warning,
    translate as _translate,
)

__all__ = [
    "apilevel",
    "threadsafety",
    "paramstyle",
    "connect",
    "connect_memory",
    "Connection",
    "Cursor",
    "RetryEvent",
    "RunResult",
    "WriteResult",
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
    "__version__",
]

#: The library version (the number ``SHOW VERSION`` prints).
__version__: str = _core.version()

#: PEP 249 globals.
apilevel = "2.0"
#: Threads may share the module, not a connection (a connection serializes
#: its calls, but one cursor's position is not meant to be shared).
threadsafety = 1
paramstyle = "qmark"

#: One retry the engine is about to make (P12): ``operation``, ``attempt``
#: (the first retry is 2), ``max_attempts``, ``error_code``, ``throttled`` and
#: ``delay_ms`` (None when unknown).
RetryEvent = _core.RetryEvent

#: The result of :meth:`Connection.run_result`: ``output`` (the text, stdout
#: and stderr merged in emission order), ``segments`` (a list of
#: ``(stream, text)`` with stream ``"stdout"`` or ``"stderr"``) and ``status``
#: (the ``ddbsh`` command's exit status: 0, or 1 when the last statement that
#: ran failed).
RunResult = namedtuple("RunResult", ["output", "segments", "status"])

#: A bulk UPDATE/UPSERT/DELETE result: ``verb`` (``"UPDATE"``, ``"UPSERT"``,
#: ``"DELETE"``), ``read``, ``modified`` and ``ccf`` (conditional-check
#: failures).
WriteResult = namedtuple("WriteResult", ["verb", "read", "modified", "ccf"])

_FETCH_CHUNK = 100


def _raise(exc: _core.EngineError) -> None:
    err = _translate(exc)
    raise err from exc.__cause__


class Cursor:
    """A PEP 249 cursor.

    ``execute`` runs one statement, or a batch of ``;``-separated statements
    without parameters. Each statement produces one or more result sets; the
    cursor starts on the first and :meth:`nextset` moves on. A SELECT's rows
    are fetched as dicts mapping attribute names to Python values (S ``str``,
    N ``decimal.Decimal``, B ``bytes``, BOOL ``bool``, NULL ``None``, L
    ``list``, M ``dict``, SS/NS/BS ``set``). A SELECT pages lazily: rows are
    read from DynamoDB as you fetch them.

    Because DynamoDB items have no fixed columns, :attr:`description` lists the
    attributes of the rows fetched so far, in the order first seen, and grows
    as you fetch. It is ``None`` for a result set without rows.

    Each cursor has its own position and its own expression-alias sequence;
    all cursors of a connection share its DynamoDB client, so a ``CONNECT``
    run through any of them retargets the connection.
    """

    #: The default number of rows :meth:`fetchmany` returns.
    arraysize: int = 1

    def __init__(self, connection: "Connection", native: Any) -> None:
        self._connection = connection
        self._native = native
        self._closed = False
        self._columns: List[str] = []
        self._seen: set = set()
        self.description: Optional[Tuple[Tuple[Any, ...], ...]] = None
        self.rowcount: int = -1

    # ---- PEP 249 ----------------------------------------------------------

    @property
    def connection(self) -> "Connection":
        """The connection this cursor belongs to."""
        return self._connection

    def execute(self, operation: str, parameters: Optional[Sequence[Any]] = None) -> "Cursor":
        """Run ``operation``. With ``parameters``, it is one statement whose
        ``?`` markers are bound in order; a value binds by its Python type (see
        the class docstring; ``float`` binds as its ``repr``, ``Decimal`` is
        exact; ``bytes``/``bytearray``/``memoryview`` bind as B; a ``set``
        binds as SS, NS or BS by its elements). Returns the cursor.

        Raises :class:`ProgrammingError` for a syntax error, a missing table,
        a wrong number of parameters or an unbindable value, and the mapped
        error (see :mod:`ddbsh._errors`) when the first statement fails. A
        later statement's error is raised by :meth:`nextset`.
        """
        self._check_open()
        if parameters is not None and (
            isinstance(parameters, (str, bytes, dict)) or not isinstance(parameters, Sequence)
        ):
            raise ProgrammingError(
                "parameters must be a sequence (paramstyle is qmark)", "invalid_argument"
            )
        self._reset_description()
        try:
            if parameters is None:
                self._native.execute(operation)
            else:
                self._native.execute_params(operation, list(parameters))
        except _core.EngineError as exc:
            _raise(exc)
        finally:
            self._update_position()
        return self

    def executemany(self, operation: str, seq_of_parameters: Sequence[Sequence[Any]]) -> None:
        """Run one statement once per parameter sequence. The statement is
        parsed once. :attr:`rowcount` is the sum of the runs' row counts, or
        -1 when any run's count is unknown."""
        total = 0
        for parameters in seq_of_parameters:
            self.execute(operation, parameters)
            total = -1 if total < 0 or self.rowcount < 0 else total + self.rowcount
        self.rowcount = total

    def fetchone(self) -> Optional[dict]:
        """The next row, or None when the result set is exhausted."""
        rows = self._fetch(1)
        return rows[0] if rows else None

    def fetchmany(self, size: Optional[int] = None) -> List[dict]:
        """Up to ``size`` (default :attr:`arraysize`) rows."""
        return self._fetch(self.arraysize if size is None else max(0, int(size)))

    def fetchall(self) -> List[dict]:
        """Every remaining row of the result set."""
        return self._fetch(-1)

    def nextset(self) -> Optional[bool]:
        """Move to the next result set: True, or None when there is none.
        Raises the error that stopped the batch."""
        self._check_open()
        self._reset_description()
        try:
            more = self._native.next_result()
        except _core.EngineError as exc:
            _raise(exc)
        finally:
            self._update_position()
        return True if more else None

    def close(self) -> None:
        """Release the cursor's results. Further use raises InterfaceError."""
        if self._closed:
            return
        self._closed = True
        if not self._connection.closed:
            try:
                self._native.close()
            except _core.EngineError as exc:
                _raise(exc)

    def setinputsizes(self, sizes: Any) -> None:
        """PEP 249; does nothing."""

    def setoutputsize(self, size: Any, column: Any = None) -> None:
        """PEP 249; does nothing."""

    def __iter__(self) -> Iterator[dict]:
        while True:
            rows = self._fetch(max(self.arraysize, _FETCH_CHUNK))
            yield from rows
            if len(rows) < max(self.arraysize, _FETCH_CHUNK):
                return

    def __enter__(self) -> "Cursor":
        return self

    def __exit__(self, *exc_info: Any) -> None:
        self.close()

    # ---- ddbsh extensions -------------------------------------------------

    @property
    def kind(self) -> Optional[str]:
        """The current result set's kind: ``"rows"``, ``"status"``,
        ``"write"``, ``"report"``, ``"explain"`` or ``"empty"``; None before
        the first execute."""
        self._check_open()
        return self._call(self._native.kind)

    @property
    def status(self) -> Optional[str]:
        """The status token of a ``status`` result set (``"CREATE"``,
        ``"INSERT"``, ``"ALTER"``, ``"ABORT"``, ...), else None."""
        self._check_open()
        token = self._call(self._native.status_token)
        return token or None

    @property
    def write_result(self) -> Optional[WriteResult]:
        """The counts of a ``write`` result set (bulk UPDATE/UPSERT/DELETE),
        else None."""
        self._check_open()
        verb, read, modified, ccf = self._call(self._native.write_counts)
        return None if verb is None else WriteResult(verb, read, modified, ccf)

    @property
    def report(self) -> List[str]:
        """The lines of a ``report`` result set (DESCRIBE, SHOW ...)."""
        self._check_open()
        return self._call(self._native.lines)

    @property
    def explain(self) -> str:
        """The serialized request text of an ``explain`` result set."""
        self._check_open()
        return self._call(self._native.explain_text)

    @property
    def diagnostics(self) -> List[str]:
        """The diagnostic lines of the current result set (a transaction's
        non-reducible-SELECT message); data, not an error."""
        self._check_open()
        return self._call(self._native.diagnostics)

    def render(self) -> str:
        """The current result set as the ``ddbsh`` command prints it (the
        remaining rows of a partly fetched SELECT)."""
        self._check_open()
        return self._call(self._native.render)

    # ---- helpers ----------------------------------------------------------

    def _call(self, fn: Callable[..., Any], *args: Any) -> Any:
        try:
            return fn(*args)
        except _core.EngineError as exc:
            _raise(exc)

    def _check_open(self) -> None:
        if self._closed:
            raise InterfaceError("cursor is closed", "interface_error")
        if self._connection.closed:
            raise InterfaceError("connection is closed", "interface_error")

    def _fetch(self, n: int) -> List[dict]:
        self._check_open()
        rows = self._call(self._native.fetch, n)
        for row in rows:
            for name in row:
                if name not in self._seen:
                    self._seen.add(name)
                    self._columns.append(name)
        if self.description is not None and len(self.description) != len(self._columns):
            self.description = tuple(
                (name, None, None, None, None, None, None) for name in self._columns
            )
        return rows

    def _reset_description(self) -> None:
        self._columns = []
        self._seen = set()
        self.description = None
        self.rowcount = -1

    def _update_position(self) -> None:
        try:
            kind = self._native.kind()
        except _core.EngineError:
            return
        if kind == "rows":
            self.description = ()
        elif kind == "write":
            _, _, modified, _ = self._native.write_counts()
            self.rowcount = modified


class Connection:
    """A connection to DynamoDB (or to the in-memory store), from
    :func:`connect` or :func:`connect_memory`. A context manager: leaving the
    ``with`` block closes it.

    One connection serializes its calls: two threads may use it, but they take
    turns. For parallel work, open one connection per thread.
    """

    Warning = Warning
    Error = Error
    InterfaceError = InterfaceError
    DatabaseError = DatabaseError
    DataError = DataError
    OperationalError = OperationalError
    IntegrityError = IntegrityError
    InternalError = InternalError
    ProgrammingError = ProgrammingError
    NotSupportedError = NotSupportedError

    def __init__(self, engine: Any, transport: Any = None) -> None:
        self._engine = engine
        self._transport = transport

    @property
    def closed(self) -> bool:
        """Whether :meth:`close` has been called."""
        return bool(self._engine.closed)

    @property
    def transport(self) -> Any:
        """The transport (a :class:`ddbsh.transport.BotocoreTransport` unless
        another was given), or None for an in-memory connection."""
        return self._transport

    @property
    def region(self) -> str:
        """The current region; ``CONNECT`` changes it."""
        return self._call(self._engine.region)

    @property
    def endpoint_overridden(self) -> bool:
        """Whether an endpoint override is in effect."""
        return self._call(self._engine.endpoint_overridden)

    def cursor(self) -> Cursor:
        """A new cursor."""
        self._check_open()
        return Cursor(self, self._engine.cursor())

    def commit(self) -> None:
        """PEP 249; does nothing. Every statement takes effect when it runs;
        group writes atomically with ``BEGIN; ... COMMIT;`` in the SQL."""
        self._check_open()

    def rollback(self) -> None:
        """PEP 249; does nothing (see :meth:`commit`)."""
        self._check_open()

    def close(self) -> None:
        """Close the connection and, when the transport has a ``close()``
        method, the transport. Further use raises InterfaceError."""
        if self._engine.closed:
            return
        self._engine.close()
        close = getattr(self._transport, "close", None)
        if callable(close):
            close()

    def run(
        self,
        sql: str,
        format: str = "default",
        json_flavor: str = "plain",
        abort_on_error: bool = False,
        on_output: Optional[Callable[[str, str], Any]] = None,
    ) -> str:
        """Run ``sql`` as ``ddbsh -c`` runs it and return the text it prints,
        stdout and stderr merged in order. Errors are part of the text, as in
        the command; see :meth:`run_result` for the exit status.

        :param format: ``"default"`` (ddbsh's own text), ``"json"``,
            ``"csv"`` or ``"table"``; a ``set output`` line in ``sql`` changes
            it as in the command.
        :param json_flavor: ``"plain"`` or ``"dynamodb"`` (typed JSON).
        :param abort_on_error: stop at the first failing statement (``-e``).
        :param on_output: called as ``on_output(stream, text)`` (stream
            ``"stdout"`` or ``"stderr"``) for each piece of output as soon as
            the engine produces it, for example to print a long Scan while it
            runs. It must not use this connection. The first exception it
            raises stops further calls and the run: the remaining statements
            send nothing to DynamoDB, and the exception is raised when the
            run ends. (With :func:`connect_memory` they still run in memory,
            with their output dropped.) A ``KeyboardInterrupt`` during the
            run stops it the same way and is raised as itself.
        :raises ValueError: an unknown format or flavor.
        """
        return self.run_result(sql, format, json_flavor, abort_on_error, on_output).output

    def run_result(
        self,
        sql: str,
        format: str = "default",
        json_flavor: str = "plain",
        abort_on_error: bool = False,
        on_output: Optional[Callable[[str, str], Any]] = None,
    ) -> RunResult:
        """:meth:`run`, returning a :data:`RunResult` with the output, its
        stdout/stderr segments and the exit status."""
        segments, status = self._call(
            self._engine.run, sql, format, json_flavor, abort_on_error, on_output
        )
        return RunResult("".join(text for _, text in segments), segments, status)

    def set_retry_callback(self, callback: Optional[Callable[[RetryEvent], Any]]) -> None:
        """Call ``callback(event)`` before each retry the engine makes (a
        throttled or transient DynamoDB error), or stop with None. The callback
        runs on the thread that made the request, before the backoff sleep; an
        exception it raises is reported through ``sys.unraisablehook`` and the
        retry still happens. It must not use this connection."""
        self._call(self._engine.set_retry_callback, callback)

    def __enter__(self) -> "Connection":
        return self

    def __exit__(self, *exc_info: Any) -> None:
        self.close()

    def _check_open(self) -> None:
        if self.closed:
            raise InterfaceError("connection is closed", "interface_error")

    def _call(self, fn: Callable[..., Any], *args: Any) -> Any:
        try:
            return fn(*args)
        except _core.EngineError as exc:
            _raise(exc)


def connect(
    session: Any = None,
    region: Optional[str] = None,
    endpoint_url: Optional[str] = None,
    verify: Union[bool, str, None] = None,
    *,
    timeout: float = 60,
    transport: Any = None,
    retry_callback: Optional[Callable[[RetryEvent], Any]] = None,
) -> Connection:
    """Connect to DynamoDB.

    :param session: the ``boto3.Session`` that supplies credentials, region,
        endpoint and CA trust; the default session when None (so
        ``AWS_PROFILE``, ``~/.aws`` profiles with ``role_arn``, and Lambda
        and container roles work as in boto3).
    :param region: the region; else the session's, else ``AWS_REGION``, else
        ``us-east-1``.
    :param endpoint_url: an endpoint override (for example a DynamoDB-compatible
        local server); else the endpoint boto3 uses for DynamoDB.
    :param verify: TLS verification: True, False or a CA bundle path; else the
        session's ``ca_bundle`` (``AWS_CA_BUNDLE``), else the default trust.
    :param timeout: the per-request timeout, in seconds.
    :param transport: an object to send requests instead of the botocore
        transport (see :mod:`ddbsh.transport`); ``session``, ``verify`` and
        ``timeout`` are then unused. The connection takes ownership of the
        transport: :meth:`Connection.close` calls its ``close()``, if it has
        one, so do not share one transport between connections.
    :param retry_callback: see :meth:`Connection.set_retry_callback`.
    """
    if transport is None:
        from .transport import BotocoreTransport

        transport = BotocoreTransport(session, region, endpoint_url, verify, timeout)
        region = transport.region
    else:
        region = region or getattr(transport, "region", None) or "us-east-1"
    conn = Connection(_core._Engine(transport, region, endpoint_url or ""), transport)
    if retry_callback is not None:
        conn.set_retry_callback(retry_callback)
    return conn


def connect_memory(region: str = "us-east-1") -> Connection:
    """An in-memory connection (the engine's ``MemoryClient``): tables and items
    live in this process, for tests and offline use. It models the data plane
    and a small part of the control plane; EXPLAIN, backups, vector search and
    SHOW LIMITS raise :class:`NotSupportedError` (or print the engine's
    not-implemented error in :meth:`Connection.run`)."""
    return Connection(_core._Engine(region), None)
