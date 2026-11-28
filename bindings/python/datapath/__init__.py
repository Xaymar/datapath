# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

"""Fast, predictable message passing between processes on the same machine.

Python bindings for the DataPath C++ library::

    import datapath

    # Server
    with datapath.host("my-service") as server:
        client = server.accept()
        client.send(client.recv())   # echo one message

    # Client (another process)
    with datapath.connect("my-service") as sock:
        sock.send(b"hello")
        print(sock.recv())
"""

from __future__ import annotations

import atexit as _atexit

from . import _core, _errors
from ._core import Context, Error, Permissions, Server, Socket, connect, host
from ._errors import (
    AccessDeniedError,
    AddressInUseError,
    ClosedError,
    DatapathError,
    InvalidPathError,
    NotFoundError,
    NotSupportedError,
    TimedOutError,
    TooLargeError,
    VersionMismatchError,
    WouldBlockError,
)

try:
    from importlib.metadata import PackageNotFoundError as _PNF
    from importlib.metadata import version as _version

    __version__ = _version("com.xaymar.datapath")
except _PNF:  # running from a build tree
    __version__ = "0+unknown"

# I/O threads must stop calling into Python before the interpreter goes away.
_atexit.register(_core._shutdown)

__all__ = [
    "Context",
    "Error",
    "Permissions",
    "Server",
    "Socket",
    "connect",
    "host",
    *_errors.__all__,
]
