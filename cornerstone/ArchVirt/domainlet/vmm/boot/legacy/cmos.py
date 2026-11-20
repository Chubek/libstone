"""Minimal CMOS RTC."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone


@dataclass(slots=True)
class CmosRtc:
    """A tiny CMOS register image."""

    base: int = 0x70
    size: int = 2

    def read(self, addr: int, size: int) -> bytes:
        if size < 0 or addr < 0 or addr + size > self.size:
            raise ValueError("CMOS access out of bounds")
        now = datetime.now(timezone.utc)
        registers = {0: now.second, 1: now.minute}
        return bytes(registers.get(addr + index, 0) for index in range(size))

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        if addr < 0 or addr + len(data) > self.size:
            raise ValueError("CMOS access out of bounds")
        return len(data)
