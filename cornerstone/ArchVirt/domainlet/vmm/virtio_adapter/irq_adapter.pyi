from dataclasses import dataclass

@dataclass
class VirtioIrqAdapter:
    sink: object
    def signal(self, vector: int = ...) -> None: ...
