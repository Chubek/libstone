"""Guest memory adapter for VirtIO domainlets."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True)
class VirtioMemoryAdapter:
    """Translate a VirtIO object into guest memory access."""

    memory: object

    def read(self, addr: int, size: int) -> bytes:
        return self.memory.read(addr, size)

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        return self.memory.write(addr, data)

    def translate(self, addr: int) -> memoryview:
        return self.memory.translate(addr)
