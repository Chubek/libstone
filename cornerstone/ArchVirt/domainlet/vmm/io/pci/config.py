"""PCI configuration space."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class PciConfigSpace:
    """A 256-byte PCI configuration space image."""

    vendor_id: int
    device_id: int
    class_code: int = 0
    subclass: int = 0
    prog_if: int = 0
    revision_id: int = 0
    header_type: int = 0
    _data: bytearray = field(
        default_factory=lambda: bytearray(256), init=False, repr=False)

    def __post_init__(self) -> None:
        for name, value, width in (
            ("vendor_id", self.vendor_id, 2),
            ("device_id", self.device_id, 2),
            ("class_code", self.class_code, 1),
            ("subclass", self.subclass, 1),
            ("prog_if", self.prog_if, 1),
            ("revision_id", self.revision_id, 1),
            ("header_type", self.header_type, 1),
        ):
            if not 0 <= value < (1 << (width * 8)):
                raise ValueError(f"{name} does not fit in {width} bytes")
        self._data[0:2] = self.vendor_id.to_bytes(2, "little")
        self._data[2:4] = self.device_id.to_bytes(2, "little")
        self._data[8] = self.revision_id & 0xFF
        self._data[9] = self.prog_if & 0xFF
        self._data[10] = self.subclass & 0xFF
        self._data[11] = self.class_code & 0xFF
        self._data[14] = self.header_type & 0xFF

    def read(self, addr: int, size: int) -> bytes:
        if addr < 0 or size < 0 or addr + size > len(self._data):
            raise ValueError("PCI configuration read out of bounds")
        return bytes(self._data[addr: addr + size])

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        payload = bytes(data)
        if addr < 0 or addr + len(payload) > len(self._data):
            raise ValueError("PCI configuration write out of bounds")
        self._data[addr: addr + len(payload)] = payload
        return len(payload)
