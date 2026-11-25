# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""The botocore transport: sign the engine's request and POST it (D-P13-5).

The engine (with its ddbsh-wire codec) builds each DynamoDB request as an AWS
JSON 1.0 body. :class:`BotocoreTransport` signs that exact body with SigV4
(botocore's ``SigV4Auth``) and sends it with botocore's HTTP session. It
returns the raw response body to the engine, including DynamoDB's ``__type``
error documents, which the codec maps to ddbsh's error text. This is the
model the browser edition uses with the AWS SDK for JavaScript.

Credentials, region, endpoint and CA trust come from the boto3 session
(D-P13-6), so profiles, AssumeRole, container and Lambda roles,
``AWS_CA_BUNDLE`` and a profile's ``ca_bundle`` work as in any Python AWS
tool. Credentials are read per request, so refreshable credentials refresh.

Each ``send`` makes exactly one attempt (D-P13-7): the engine retries
throttling and transient errors itself and raises the P12 retry event.

A transport object is any object with these three methods, and optionally
``close()``; tests pass their own (see ``ddbsh.connect(transport=...)``):

``send(operation, body) -> str``
    POST ``body`` for ``operation`` (``"Query"``, ``"PutItem"``, ...) and
    return the response body, for any HTTP status that carries a DynamoDB
    response. Raise on a transport failure; the engine reports
    ``wire transport: <TypeName>: <message>``.
``on_connect(region, endpoint)``
    ``CONNECT <region> [WITH ENDPOINT <endpoint>]`` ran; retarget. An empty
    endpoint means the default endpoint of ``region``.
``descriptor() -> str``
    The ``Backend:`` text of ``SHOW CONNECTION``.
``close()`` (optional)
    Release the transport's resources. The connection owns its transport:
    ``Connection.close()`` calls ``close()`` once when the transport has it,
    so give each connection its own transport object.
"""

from __future__ import annotations

import json
import os
import re
from typing import Any, Optional, Union
from urllib.parse import urlsplit, urlunsplit

import botocore
import botocore.session
from botocore.auth import SigV4Auth
from botocore.awsrequest import AWSRequest
from botocore.exceptions import NoCredentialsError
from botocore.httpsession import URLLib3Session
from botocore.utils import get_environ_proxies

from . import _core

#: The JSON 1.0 target prefix of every DynamoDB operation.
TARGET_PREFIX = "DynamoDB_20120810."

#: The default request timeout, in seconds (botocore's default).
DEFAULT_TIMEOUT = 60

#: The operations DynamoDB serves from its search endpoint.
SEARCH_OPERATIONS = frozenset({"SearchVectors"})

# An AWS DynamoDB service host: dynamodb[-fips].<region>.<amazonaws.com|api.aws>
# (and the .cn partition's amazonaws.com.cn).
_SERVICE_HOST = re.compile(r"^(dynamodb(?:-fips)?)\.([a-z0-9-]+)\.(amazonaws\.com(?:\.cn)?|api\.aws)$")


class TransportError(Exception):
    """A request could not be sent, or the reply was not a DynamoDB response."""


def mask_access_key_id(access_key_id: str) -> str:
    """The first and last four characters of a key id, as the browser edition
    shows it (``ASIA…WXYZ``); never the secret or the token."""
    if len(access_key_id) <= 8:
        return "…"
    return f"{access_key_id[:4]}…{access_key_id[-4:]}"


def _botocore_session(session: Any) -> botocore.session.Session:
    # A boto3.Session wraps a botocore session as `_session` (boto3's own
    # clients use it the same way); a botocore session is accepted as is.
    if session is None:
        import boto3

        session = boto3.Session()
    return getattr(session, "_session", session)


def resolve_region(session: Any, region: Optional[str] = None) -> str:
    """The region a connection starts in: ``region``, else the session's
    (``AWS_DEFAULT_REGION`` or the profile's ``region``), else
    ``AWS_REGION``, else ``us-east-1``, the ``ddbsh`` command's default."""
    if region:
        return region
    bc = _botocore_session(session)
    return bc.get_config_variable("region") or os.environ.get("AWS_REGION") or "us-east-1"


def search_endpoint(url: str) -> str:
    """The search endpoint that serves SearchVectors for the DynamoDB service
    endpoint ``url``: ``https://dynamodb.us-east-1.amazonaws.com`` ->
    ``https://search-dynamodb.us-east-1.amazonaws.com`` (the endpoint rules'
    ``IsSearchOperation``; the AWS SDKs route it the same way). Any other
    endpoint (an override, a local server) serves every operation itself and is
    returned unchanged."""
    parts = urlsplit(url)
    if parts.port is None and _SERVICE_HOST.match(parts.hostname or ""):
        return urlunsplit(parts._replace(netloc="search-" + parts.netloc))
    return url


def _with_scheme(endpoint: str) -> str:
    # The ddbsh command's AWS SDK for C++ assumes https:// for an endpoint
    # given without a scheme; do the same.
    return endpoint if "://" in endpoint else "https://" + endpoint


class BotocoreTransport:
    """Sign each request with SigV4 and POST it with botocore's HTTP session.

    :param session: a ``boto3.Session`` (or a botocore session); the default
        session when None.
    :param region: the initial region (see :func:`resolve_region`).
    :param endpoint_url: an endpoint override, e.g. ``http://localhost:8000``.
        When None, the endpoint is the one boto3 would use for DynamoDB in
        the region (``AWS_ENDPOINT_URL_DYNAMODB`` and a profile's
        ``endpoint_url`` included). Only an endpoint given here (or by
        ``CONNECT ... WITH ENDPOINT``) counts as an override: one boto3
        resolved from its configuration is reported as ``(default)`` by
        ``SHOW CONNECTION``, and :attr:`endpoint_overridden` is False.
    :param verify: TLS verification: True, False, or a CA bundle path. When
        None, the session's ``ca_bundle`` (``AWS_CA_BUNDLE`` or the profile's
        ``ca_bundle``), else the default trust.
    :param timeout: the connect and read timeout per request, in seconds.
    """

    def __init__(
        self,
        session: Any = None,
        region: Optional[str] = None,
        endpoint_url: Optional[str] = None,
        verify: Union[bool, str, None] = None,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        self._session = _botocore_session(session)
        if verify is None:
            verify = self._session.get_config_variable("ca_bundle") or True
        self._verify = verify
        self._timeout = timeout
        self._user_agent = (
            f"ddbsh/{_core.version()} (python) botocore/{botocore.__version__}"
        )
        self.region = resolve_region(self._session, region)
        self.endpoint = ""
        self.search_endpoint = ""
        self.endpoint_overridden = False
        self._http: Optional[URLLib3Session] = None
        self._search_http: Optional[URLLib3Session] = None
        self._closed = False
        self._retarget(self.region, endpoint_url or "")

    # ---- the transport interface ------------------------------------------

    def send(self, operation: str, body: str) -> str:
        """POST ``body`` for ``operation``; return the response body."""
        credentials = self._session.get_credentials()
        if credentials is None:
            raise NoCredentialsError()
        frozen = credentials.get_frozen_credentials()
        search = operation in SEARCH_OPERATIONS
        request = AWSRequest(
            method="POST",
            url=self.search_endpoint if search else self.endpoint,
            data=body.encode("utf-8"),
            headers={
                "Content-Type": "application/x-amz-json-1.0",
                "X-Amz-Target": TARGET_PREFIX + operation,
                "User-Agent": self._user_agent,
            },
        )
        SigV4Auth(frozen, "dynamodb", self.region).add_auth(request)
        http = self._search_http if search else self._http
        if http is None:
            raise TransportError("the transport is closed")
        response = http.send(request.prepare())
        text = response.content.decode("utf-8", errors="replace")
        if response.status_code != 200 and not _is_service_error(text):
            raise TransportError(
                f"HTTP {response.status_code} from {request.url} without a DynamoDB error"
                " document"
            )
        return text

    def on_connect(self, region: str, endpoint: str) -> None:
        """Retarget after ``CONNECT``. Raises :class:`TransportError` once the
        transport is closed."""
        if self._closed:
            raise TransportError("the transport is closed")
        self._retarget(region, endpoint)

    def close(self) -> None:
        """Close the HTTP connection pools (``Connection.close`` calls this).
        A closed transport stays closed: a later :meth:`send` or
        :meth:`on_connect` raises :class:`TransportError`."""
        self._closed = True
        self._close_http()

    def descriptor(self) -> str:
        """The ``SHOW CONNECTION`` backend text, e.g.
        ``wire/botocore 1.35.0 -> https://dynamodb.us-east-1.amazonaws.com
        (default); credentials ASIA…WXYZ``."""
        target = self.endpoint if self.endpoint_overridden else f"{self.endpoint} (default)"
        credentials = self._session.get_credentials()
        if credentials is None:
            cred = "credentials none"
        else:
            cred = f"credentials {mask_access_key_id(credentials.get_frozen_credentials().access_key)}"
        return f"wire/botocore {botocore.__version__} -> {target}; {cred}"

    # ---- helpers ----------------------------------------------------------

    def _retarget(self, region: str, endpoint: str) -> None:
        if endpoint:
            url = _with_scheme(endpoint)
            overridden = True
        else:
            client = self._session.create_client("dynamodb", region_name=region)
            url = client.meta.endpoint_url
            overridden = False
        self.region = region
        self.endpoint = url
        self.search_endpoint = url if overridden else search_endpoint(url)
        self.endpoint_overridden = overridden
        self._close_http()
        self._http = self._new_http(url)
        # The search endpoint is another host: NO_PROXY may treat it apart.
        if self.search_endpoint == url or get_environ_proxies(
            self.search_endpoint
        ) == get_environ_proxies(url):
            self._search_http = self._http
        else:
            self._search_http = self._new_http(self.search_endpoint)

    def _new_http(self, url: str) -> URLLib3Session:
        return URLLib3Session(
            verify=self._verify,
            timeout=self._timeout,
            proxies=get_environ_proxies(url),
        )

    def _close_http(self) -> None:
        if self._search_http is not None and self._search_http is not self._http:
            self._search_http.close()
        if self._http is not None:
            self._http.close()
        self._http = None
        self._search_http = None


def _is_service_error(text: str) -> bool:
    try:
        doc = json.loads(text)
    except ValueError:
        return False
    return isinstance(doc, dict) and "__type" in doc
