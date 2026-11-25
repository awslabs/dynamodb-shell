# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""The `libddbsh` alias (python/alias, D-P13-4): it re-exports `ddbsh`, and
its version and `ddbsh==` pin equal the project version."""

from __future__ import annotations

import importlib
import re
import sys
from pathlib import Path

import pytest

import ddbsh

REPO = Path(__file__).resolve().parents[2]
ALIAS_DIR = REPO / "python" / "alias"


def _cmake_version() -> str:
    text = (REPO / "CMakeLists.txt").read_text()
    m = re.search(r"project\(ddbsh\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text)
    assert m, "no project(ddbsh VERSION ...) in CMakeLists.txt"
    return m.group(1)


@pytest.fixture()
def libddbsh(monkeypatch: pytest.MonkeyPatch):
    """The alias module: the installed one when the alias wheel is installed,
    else the one in python/alias."""
    try:
        module = importlib.import_module("libddbsh")
    except ImportError:
        module = None
    if module is not None:
        yield module
        return
    monkeypatch.syspath_prepend(str(ALIAS_DIR))
    sys.modules.pop("libddbsh", None)
    module = importlib.import_module("libddbsh")
    yield module
    sys.modules.pop("libddbsh", None)


def test_alias_exposes_the_same_objects(libddbsh) -> None:
    assert libddbsh.__all__ == ddbsh.__all__
    for name in ddbsh.__all__:
        assert getattr(libddbsh, name) is getattr(ddbsh, name), name
    assert libddbsh.__version__ == ddbsh.__version__


def test_alias_works_as_ddbsh(libddbsh) -> None:
    with libddbsh.connect_memory() as conn:
        conn.run("create table t ( id number ) primary key ( id hash );")
        conn.run("insert into t ( id ) values ( 1 );")
        assert conn.run("select * from t;") == "{id: 1}\n"
        with pytest.raises(ddbsh.ProgrammingError):
            conn.cursor().execute("select from;")


def test_versions_agree() -> None:
    version = _cmake_version()
    # SHOW VERSION drops a trailing ".0" (0.13.0 prints 0.13; src/version.cpp).
    shown = version[: -len(".0")] if version.endswith(".0") else version
    assert ddbsh.__version__ == shown
    alias = (ALIAS_DIR / "pyproject.toml").read_text()
    assert re.search(rf'^version = "{re.escape(version)}"$', alias, re.M), (
        f"python/alias/pyproject.toml version is not {version}"
    )
    assert re.search(rf'^dependencies = \["ddbsh=={re.escape(version)}"\]$', alias, re.M), (
        f"python/alias/pyproject.toml does not pin ddbsh=={version}"
    )


def test_alias_license_is_the_project_license() -> None:
    # python/alias/LICENSE is a copy, so the alias's own wheel and sdist carry
    # it (a build backend takes license files from the project directory).
    assert (ALIAS_DIR / "LICENSE").read_bytes() == (REPO / "LICENSE").read_bytes()
