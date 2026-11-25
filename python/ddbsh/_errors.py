# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""The PEP 249 exception hierarchy and the mapping from engine errors.

Every exception carries ddbsh's exact error text (the line the ``ddbsh``
command prints) as ``str(exc)``, the engine's error code as ``exc.code`` and,
for a DynamoDB service error, the exception name (for example
``"ResourceNotFoundException"``) as ``exc.service_error``. An exception the
transport raised (a network failure, missing credentials) is the
``__cause__``.
"""

from __future__ import annotations

from typing import Optional

from . import _core


class Warning(Exception):  # noqa: A001 - the PEP 249 name
    """PEP 249 Warning. ddbsh raises none; it exists for DB-API compatibility."""


class Error(Exception):
    """Base class of every ddbsh error (PEP 249)."""

    def __init__(self, message: str, code: str = "", service_error: Optional[str] = None):
        super().__init__(message)
        self.message = message
        self.code = code
        self.service_error = service_error


class InterfaceError(Error):
    """The connection or cursor was used wrongly (for example after close())."""


class DatabaseError(Error):
    """An error reported by the engine or by DynamoDB."""


class DataError(DatabaseError):
    """A value could not be processed."""


class OperationalError(DatabaseError):
    """A service or transport failure: throttling after retries, a network
    error, a cancelled transaction, any DynamoDB error not mapped elsewhere."""


class IntegrityError(DatabaseError):
    """A conditional check failed (for example INSERT of an existing key)."""


class InternalError(DatabaseError):
    """An internal engine error."""


class ProgrammingError(DatabaseError):
    """A statement error: a syntax error, a missing table, a wrong parameter
    count or an unbindable parameter value."""


class NotSupportedError(DatabaseError):
    """The operation is not supported by this backend (for example EXPLAIN on
    the in-memory connection)."""


_BY_CODE = {
    "parse_error": ProgrammingError,
    "no_such_table": ProgrammingError,
    "table_already_exists": ProgrammingError,
    "invalid_argument": ProgrammingError,
    "unbound_parameter": ProgrammingError,
    "not_reducible_to_getitem": ProgrammingError,
    "no_result_set": ProgrammingError,
    "reentrant_call": ProgrammingError,
    "conditional_check_failed": IntegrityError,
    "service_error": OperationalError,
    "cancelled": OperationalError,
    "not_implemented": NotSupportedError,
    "internal_error": InternalError,
    "interface_error": InterfaceError,
}


# DynamoDB exception names that pick a more specific class than the engine
# code does. On the wire path a service error arrives as code "service_error"
# with the exception name; these map as the in-memory path's codes do.
_BY_SERVICE_ERROR = {
    "ResourceNotFoundException": ProgrammingError,
    "ResourceInUseException": ProgrammingError,
    "ValidationException": ProgrammingError,
    "ConditionalCheckFailedException": IntegrityError,
    "InternalServerError": InternalError,
}


def translate(exc: _core.EngineError) -> Error:
    """The PEP 249 exception for an engine error (the caller attaches the
    cause)."""
    code, message, service_error = exc.args
    cls = _BY_SERVICE_ERROR.get(service_error or "") or _BY_CODE.get(code, DatabaseError)
    return cls(message, code, service_error)
