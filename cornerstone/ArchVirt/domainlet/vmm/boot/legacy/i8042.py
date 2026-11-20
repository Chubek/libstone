"""Minimal i8042 keyboard controller."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class I8042:
    """Small but stateful i8042 keyboard controller model."""

    base: int = 0x60
    size: int = 4
    _status: int = field(default=0, init=False, repr=False)
    _output: bytearray = field(
        default_factory=bytearray, init=False, repr=False)
    _command: int = field(default=0, init=False, repr=False)

    def read(self, addr: int, size: int) -> bytes:
        if size <= 0:
            return b""
        value = bytearray(size)
        for index in range(size):
            offset = addr + index
            if offset in (0, 4):
                value[index] = self._status | (1 if self._output else 0)
            elif offset in (1, 5) and self._output:
                value[index] = self._output.pop(0)
        return bytes(value)

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        payload = bytes(data)
        for index, item in enumerate(payload):
            offset = addr + index
            if offset in (0, 4):
                self._command = item
                if item == 0xFE:  # CPU reset command: expose an acknowledgement.
                    self._output.append(0xFA)
            elif offset in (1, 5):
                self._output.append(item)
        self._status = 0
        return len(payload)
