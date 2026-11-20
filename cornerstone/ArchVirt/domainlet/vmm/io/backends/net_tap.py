"""TAP network backend."""

from __future__ import annotations

import fcntl
import os
import socket
import struct
from dataclasses import dataclass, field

from domainlet.vmm.errors import VmmError

TUNSETIFF = 0x400454CA
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000


@dataclass(slots=True)
class TapBackend:
    """A TAP device backed by `/dev/net/tun`."""

    name: str = "tap0"
    fd: int = field(init=False)

    def __post_init__(self) -> None:
        try:
            self.fd = os.open("/dev/net/tun", os.O_RDWR | os.O_CLOEXEC)
            ifr = struct.pack("16sH", self.name.encode(
                "ascii"), IFF_TAP | IFF_NO_PI)
            fcntl.ioctl(self.fd, TUNSETIFF, ifr)
        except OSError as exc:
            raise VmmError(f"unable to open TAP backend: {exc}") from exc

    def fileno(self) -> int:
        return self.fd

    def recv(self, size: int = 65536) -> bytes:
        return os.read(self.fd, size)

    def send(self, data: bytes | bytearray | memoryview) -> int:
        return os.write(self.fd, bytes(data))

    def close(self) -> None:
        os.close(self.fd)
