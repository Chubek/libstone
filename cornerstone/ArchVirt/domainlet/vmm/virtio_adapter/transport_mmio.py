"""VirtIO MMIO transport window."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True)
class VirtioMmioTransport:
    """A tiny MMIO transport window."""

    base: int
    size: int
    device: object

    def read(self, addr: int, size: int) -> bytes:
        return self.device.read(addr, size)

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        return self.device.write(addr, data)
