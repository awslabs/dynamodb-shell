# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
"""``libddbsh`` is another name for the ``ddbsh`` package (D-P13-4).

``import libddbsh`` gives the same objects as ``import ddbsh``; the two names
can be mixed in one program. New code should use ``ddbsh``.
"""

from ddbsh import *  # noqa: F401,F403 (the alias re-exports ddbsh's API)
from ddbsh import __all__, __version__  # noqa: F401
