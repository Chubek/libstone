"""eventfd wrapper."""

from __future__ import annotations

import os
from dataclasses import dataclass, field


@dataclass(slots=True)
class EventFd:
    """Small wrapper around ``os.eventfd``."""

    flags: int = 0
    fd: int = field(init=False)
    _r: int = field(init=False, repr=False)
    _w: int = field(init=False, repr=False)

    def __post_init__(self) -> None:
        if self.flags < 0:
            raise ValueError("eventfd flags must be non-negative")
        if hasattr(os, "eventfd"):
            self.fd = os.eventfd(0, self.flags | getattr(os, "EFD_CLOEXEC", 0))
        else:
            self._r, self._w = os.pipe()
            self.fd = self._r

    def fileno(self) -> int:
        return self.fd

    def read(self) -> int:
        fd = self.fd if hasattr(os, "eventfd") else self._r
        payload = os.read(fd, 8)
        if len(payload) != 8:
            raise OSError("short eventfd read")
        return int.from_bytes(payload, "little")

    def write(self, value: int = 1) -> None:
        if value < 0 or value > 0xFFFFFFFFFFFFFFFF:
            raise ValueError("eventfd value must fit in uint64")
        payload = int(value).to_bytes(8, "little")
        if hasattr(os, "eventfd"):
            os.write(self.fd, payload)
        else:
            os.write(self._w, payload)

    def close(self) -> None:
        if getattr(self, "fd", -1) < 0:
            return
        if hasattr(os, "eventfd"):
            os.close(self.fd)
            self.fd = -1
        else:
            os.close(self._r)
            os.close(self._w)
            self.fd = -1
