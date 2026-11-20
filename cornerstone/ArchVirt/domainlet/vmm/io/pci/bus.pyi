from dataclasses import dataclass

from domainlet.vmm.io.pci.device import PciConfigDevice

@dataclass(frozen=True, slots=True)
class PciAddress:
    bus: int
    device: int
    function: int

@dataclass
class PciRootBus:
    devices: dict[PciAddress, PciConfigDevice]
    def register(self, address: PciAddress, device: PciConfigDevice) -> None: ...
    def read_config(self, address: PciAddress, offset: int, size: int) -> bytes: ...
    def write_config(self, address: PciAddress, offset: int, data: bytes | bytearray | memoryview) -> int: ...
