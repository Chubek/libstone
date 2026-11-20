"""Network backend seam."""

from __future__ import annotations

from typing import Protocol


class NetBackend(Protocol):
    """A packet-oriented network backend."""

    def recv(self, size: int = ...) -> bytes: ...

    def send(self, data: bytes | bytearray | memoryview) -> int: ...

    def fileno(self) -> int: ...

    def close(self) -> None: ...
