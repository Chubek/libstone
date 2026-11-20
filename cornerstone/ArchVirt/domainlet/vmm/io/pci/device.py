"""PCI device seam."""

from __future__ import annotations

from typing import Protocol

from domainlet.vmm.io.pci.config import PciConfigSpace


class PciDevice(Protocol):
    """A device with PCI configuration space."""

    config: PciConfigSpace


class PciConfigDevice:
    """BusDevice wrapper over PCI config space."""

    def __init__(self, config: PciConfigSpace) -> None:
        self.config = config
        self.base = 0
        self.size = 256

    def read(self, addr: int, size: int) -> bytes:
        return self.config.read(addr, size)

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        return self.config.write(addr, data)
