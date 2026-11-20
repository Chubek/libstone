from dataclasses import dataclass

@dataclass
class VirtioBusAdapter:
    device: object
    base: int
    size: int
    def read(self, addr: int, size: int) -> bytes: ...
    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int: ...
