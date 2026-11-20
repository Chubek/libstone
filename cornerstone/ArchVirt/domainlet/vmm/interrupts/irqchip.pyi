from dataclasses import dataclass
from typing import Protocol

class IrqChip(Protocol):
    def signal(self, vector: int = ...) -> None: ...

@dataclass
class KvmIrqChip:
    vm: object
    def __post_init__(self) -> None: ...
    def signal(self, vector: int = ...) -> None: ...
