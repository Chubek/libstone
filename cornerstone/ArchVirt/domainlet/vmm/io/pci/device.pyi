from typing import Protocol

from domainlet.vmm.io.pci.config import PciConfigSpace

class PciDevice(Protocol):
    config: PciConfigSpace

class PciConfigDevice:
    config: PciConfigSpace
    base: int
    size: int
    def __init__(self, config: PciConfigSpace) -> None: ...
    def read(self, addr: int, size: int) -> bytes: ...
    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int: ...
