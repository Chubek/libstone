"""Minimal 16550-compatible serial port."""

from __future__ import annotations

from dataclasses import dataclass, field

from domainlet.vmm.boot.legacy.console_sink import ConsoleSink


@dataclass(slots=True)
class SerialPort:
    """A tiny UART front end."""

    base: int = 0x3F8
    size: int = 8
    sink: ConsoleSink = field(default_factory=ConsoleSink)
    buffer: bytearray = field(default_factory=bytearray, repr=False)

    def read(self, addr: int, size: int) -> bytes:
        if addr < 0 or size < 0 or addr + size > self.size:
            raise ValueError("serial access out of bounds")
        result = bytearray(size)
        for index in range(size):
            register = addr + index
            if register == 0 and self.buffer:
                result[index] = self.buffer.pop(0)
            elif register == 5:
                result[index] = 0x60 | (1 if self.buffer else 0)
            elif register == 6:
                result[index] = 0
        return bytes(result)

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        payload = bytes(data)
        if addr < 0 or addr + len(payload) > self.size:
            raise ValueError("serial access out of bounds")
        for index, byte in enumerate(payload):
            if addr + index == 0:
                self.sink.write(bytes((byte,)))
        return len(payload)
