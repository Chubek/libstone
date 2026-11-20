from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class KvmCapabilities:
    values: dict[int, int]
    def require(self, capability: int) -> int: ...

def probe_capabilities(fd: int) -> KvmCapabilities: ...
