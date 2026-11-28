# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

"""Exceptions raised by DataPath.

Every exception derives from :class:`DatapathError` and carries the native error
code in ``.error``. Each one also derives from the closest built-in exception,
so ``except ConnectionError`` or ``except TimeoutError`` work as expected.
"""

from __future__ import annotations

__all__ = [
    "DatapathError",
    "ClosedError",
    "TimedOutError",
    "InvalidPathError",
    "NotSupportedError",
    "TooLargeError",
    "WouldBlockError",
    "VersionMismatchError",
    "NotFoundError",
    "AddressInUseError",
    "AccessDeniedError",
]


class DatapathError(Exception):
    """Base class of all DataPath errors. ``.error`` holds the :class:`datapath.Error` code."""

    error: object = None


class ClosedError(DatapathError, ConnectionError):
    """The connection or server is closed."""


class TimedOutError(DatapathError, TimeoutError):
    """The operation did not complete in time."""


class InvalidPathError(DatapathError, ValueError):
    """The path is malformed or too long for the platform."""


class NotSupportedError(DatapathError):
    """The operation is not supported on this platform."""


class TooLargeError(DatapathError, ValueError):
    """The message exceeds the connection's negotiated maximum message size."""


class WouldBlockError(DatapathError, BlockingIOError):
    """The send queue is full (``max_queued_bytes``); retry later."""


class VersionMismatchError(DatapathError):
    """The peer speaks a different protocol version."""


class NotFoundError(DatapathError, FileNotFoundError):
    """Nothing is listening at the path."""


class AddressInUseError(DatapathError, OSError):
    """Another server already hosts this path."""


class AccessDeniedError(DatapathError, PermissionError):
    """The server's permissions don't allow this user to connect."""
