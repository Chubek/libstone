"""Bus adapter for VirtIO domainlets."""

from __future__ import annotations

from dataclasses import dataclass

from domainlet.vmm.seams import BusDevice


@dataclass(slots=True)
class VirtioBusAdapter:
    """Translate a VirtIO object into a bus device."""

    device: object
    base: int
    size: int

    def read(self, addr: int, size: int) -> bytes:
        return self.device.read(addr, size)

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        return self.device.write(addr, data)
