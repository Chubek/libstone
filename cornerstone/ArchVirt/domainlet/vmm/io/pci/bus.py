"""PCI root bus."""

from __future__ import annotations

from dataclasses import dataclass, field

from domainlet.vmm.errors import BusError
from domainlet.vmm.io.pci.device import PciConfigDevice


@dataclass(frozen=True, slots=True)
class PciAddress:
    """A PCI function address."""

    bus: int
    device: int
    function: int


@dataclass(slots=True)
class PciRootBus:
    """A tiny PCI registry."""

    devices: dict[PciAddress, PciConfigDevice] = field(default_factory=dict)

    def register(self, address: PciAddress, device: PciConfigDevice) -> None:
        if not (0 <= address.bus <= 255 and 0 <= address.device <= 31 and 0 <= address.function <= 7):
            raise BusError(f"invalid PCI address {address}")
        if address in self.devices:
            raise BusError(f"PCI device already registered at {address}")
        self.devices[address] = device

    def read_config(self, address: PciAddress, offset: int, size: int) -> bytes:
        try:
            return self.devices[address].read(offset, size)
        except KeyError as exc:
            raise BusError(f"unknown PCI device {address}") from exc

    def write_config(self, address: PciAddress, offset: int, data: bytes | bytearray | memoryview) -> int:
        try:
            return self.devices[address].write(offset, data)
        except KeyError as exc:
            raise BusError(f"unknown PCI device {address}") from exc
