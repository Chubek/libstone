"""Block backend seam."""

from __future__ import annotations

from typing import Protocol


class BlockBackend(Protocol):
    """Read/write block device backend."""

    def read(self, offset: int, size: int) -> bytes: ...

    def write(self, offset: int, data: bytes |
              bytearray | memoryview) -> int: ...

    def close(self) -> None: ...
