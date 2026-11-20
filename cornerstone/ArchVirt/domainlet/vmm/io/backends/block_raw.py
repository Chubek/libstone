"""Raw file block backend."""

from __future__ import annotations

import os
from dataclasses import dataclass, field

from domainlet.vmm.errors import VmmError


@dataclass(slots=True)
class RawBlockBackend:
    """A file-backed raw block device."""

    path: str
    read_only: bool = False
    fd: int = field(init=False)

    def __post_init__(self) -> None:
        flags = os.O_RDONLY if self.read_only else os.O_RDWR
        try:
            self.fd = os.open(self.path, flags | os.O_CLOEXEC)
        except OSError as exc:
            raise VmmError(
                f"unable to open block backend {self.path}: {exc}") from exc

    def read(self, offset: int, size: int) -> bytes:
        return os.pread(self.fd, size, offset)

    def write(self, offset: int, data: bytes | bytearray | memoryview) -> int:
        if self.read_only:
            raise VmmError("block backend is read-only")
        return os.pwrite(self.fd, bytes(data), offset)

    def close(self) -> None:
        os.close(self.fd)
