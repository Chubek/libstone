from dataclasses import dataclass

@dataclass
class VirtioMmioTransport:
    base: int
    size: int
    device: object
    def read(self, addr: int, size: int) -> bytes: ...
    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int: ...
