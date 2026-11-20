"""Address-range dispatch for MMIO and PIO."""

from __future__ import annotations

from dataclasses import dataclass, field

from domainlet.vmm.errors import BusError
from domainlet.vmm.seams import BusDevice


@dataclass(frozen=True, slots=True)
class BusMapping:
    """A registered device window."""

    base: int
    size: int
    device: BusDevice

    def contains(self, addr: int, size: int = 1) -> bool:
        return size >= 0 and self.base <= addr and addr + size <= self.base + self.size


@dataclass(slots=True)
class Bus:
    """A first-fit address bus."""

    mappings: list[BusMapping] = field(default_factory=list)

    def register(self, base: int, size: int, device: BusDevice) -> None:
        """Register a new device window."""

        if base < 0 or size <= 0:
            raise BusError("bus window size must be positive")
        end = base + size
        if end <= base:
            raise BusError("bus window overflows address space")
        for mapping in self.mappings:
            if base < mapping.base + mapping.size and mapping.base < end:
                raise BusError(f"bus window overlaps {mapping.base:#x}")
        self.mappings.append(BusMapping(base, size, device))
        self.mappings.sort(key=lambda item: item.base)

    def resolve(self, addr: int, size: int = 1) -> BusMapping:
        """Find the mapping covering an address."""

        for item in self.mappings:
            if item.contains(addr, size):
                return item
        raise BusError(f"no device registered for address {addr:#x}")

    def read(self, addr: int, size: int) -> bytes:
        """Read from the bus."""

        if size < 0:
            raise BusError("read size cannot be negative")
        mapping = self.resolve(addr, size)
        result = bytes(mapping.device.read(addr - mapping.base, size))
        if len(result) != size:
            raise BusError(
                f"device returned {len(result)} bytes for {size}-byte read")
        return result

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        """Write to the bus."""

        mapping = self.resolve(addr, len(data))
        result = mapping.device.write(addr - mapping.base, data)
        if result != len(data):
            raise BusError(
                f"device accepted {result} bytes for {len(data)}-byte write")
        return result

    def dispatch(
        self,
        addr: int,
        data: bytes | bytearray | memoryview | None = None,
        *,
        write: bool = False,
        size: int | None = None,
    ) -> bytes | int:
        """Route an MMIO or PIO access."""

        if write:
            if data is None:
                raise BusError("write dispatch requires data")
            return self.write(addr, data)
        return self.read(addr, len(data) if data is not None else (size or 1))


class MmioBus(Bus):
    """Bus for guest physical MMIO space."""


class PioBus(Bus):
    """Bus for port I/O space."""
