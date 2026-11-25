# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""``python -m ddbsh``: a ``ddbsh``-shaped front end over the Python package.

It runs ``-c`` text, a ``-f`` file or standard input through
:meth:`ddbsh.Connection.run_result` (the CLI's own ``-c`` driver) and writes
each piece of output to stdout or stderr as the engine emits it, with the
``ddbsh`` command's exit status. The golden suite drives the Python edition
through it exactly as it drives the native binary (devtools/run-python-golden;
tests/golden/golden-runner.sh, DDBSH_SHELL_BIN). It is not an interactive
shell: there is no prompt, history or line editing (use the ``ddbsh``
command for that).

Options (the subset of the ``ddbsh`` command the golden runner uses):

  -c <cmds>   run a command string       -f <file>   run a file
  -e          stop at the first error    (neither -c nor -f: read stdin)
  -q, -d, -s  accepted; there is no banner, -d is a no-op in the command too,
              and the golden runner sorts output itself
  --output <format>, --json-flavor <flavor>   (also --opt=value)

The connection is :func:`ddbsh.connect` with the default boto3 session;
``DDBSH_ENDPOINT_OVERRIDE`` sets ``endpoint_url`` as it overrides the
command's endpoint. No config file (``~/.ddbsh_config``) is read.
"""

from __future__ import annotations

import os
import sys
from typing import List, NoReturn, Optional

#: Byte-identical to the ``ddbsh`` command's usage line (cli/main.cpp kUsage),
#: so a golden that pins a usage error matches in every edition.
USAGE = (
    "Usage: ddbsh [-q] [-e] [-d] [-t[TERM]] [--ca-bundle <file>] [--show-retries] "
    "[--output default|json|csv|table] [--json-flavor plain|dynamodb] "
    '[-c "<cmds>" | -f <file>]\n'
)

_OUTPUT_FORMATS = ("default", "json", "csv", "table")
_JSON_FLAVORS = ("plain", "dynamodb")


def _write(fd: int, text: str) -> None:
    data = text.encode("utf-8")
    while data:
        n = os.write(fd, data)
        data = data[n:]


def _die(message: str, code: int = 1) -> NoReturn:
    _write(2, message)
    sys.exit(code)


def _check(kind: str, value: str, known: tuple) -> None:
    if value.lower() in known:
        return
    if kind == "output":
        message = f"unknown output format: {value} (expected default, json, csv, table)\n"
    else:
        message = f"unknown JSON flavor: {value} (expected plain, dynamodb)\n"
    _die(f"ddbsh: {message}{USAGE}")


def main(argv: Optional[List[str]] = None) -> int:
    """Run the front end; returns the exit status."""
    args = sys.argv[1:] if argv is None else argv
    cmdstring: Optional[str] = None
    inputfile: Optional[str] = None
    abort_on_error = False
    output = "default"
    flavor = "plain"

    i = 0
    while i < len(args):
        a = args[i]
        if not a.startswith("-") or a == "-":
            _die(USAGE)
        if a.startswith("--"):
            name, eq, value = a[2:].partition("=")
            if name not in ("output", "json-flavor"):
                _die(USAGE)
            if not eq:
                i += 1
                if i >= len(args):
                    _die(USAGE)
                value = args[i]
            if name == "output":
                _check("output", value, _OUTPUT_FORMATS)
                output = value
            else:
                _check("json-flavor", value, _JSON_FLAVORS)
                flavor = value
            i += 1
            continue
        j = 1
        while j < len(a):
            ch = a[j]
            if ch in "qds":
                j += 1
                continue
            if ch == "e":
                abort_on_error = True
                j += 1
                continue
            if ch in "cf":
                value = a[j + 1 :]
                if value == "":
                    i += 1
                    if i >= len(args):
                        _die(USAGE)
                    value = args[i]
                if ch == "c":
                    cmdstring = value
                else:
                    inputfile = value
                break
            _die(USAGE)
        i += 1

    if cmdstring is not None and inputfile is not None:
        # The command prints this to stdout and exits 1.
        _write(1, "ddbsh: Cannot specify both -c and -f.\n")
        return 1

    if cmdstring is not None:
        text = cmdstring
    elif inputfile is not None:
        try:
            with open(inputfile, encoding="utf-8") as f:
                text = f.read()
        except OSError as e:
            # perror(<path>) parity: "<path>: <strerror>".
            _die(f"{inputfile}: {os.strerror(e.errno) if e.errno else e}\n")
    else:
        text = sys.stdin.read()

    import ddbsh

    endpoint = os.environ.get("DDBSH_ENDPOINT_OVERRIDE") or None
    conn = ddbsh.connect(endpoint_url=endpoint)
    # Write each piece as the engine emits it, as the command does.
    result = conn.run_result(
        text,
        output,
        flavor,
        abort_on_error,
        on_output=lambda stream, piece: _write(2 if stream == "stderr" else 1, piece),
    )
    return result.status


if __name__ == "__main__":
    sys.exit(main())
