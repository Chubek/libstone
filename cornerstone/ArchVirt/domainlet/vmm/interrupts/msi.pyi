from dataclasses import dataclass

@dataclass
class MsiRouteTable:
    routes: dict[int, int]
    def add(self, vector: int, gsi: int) -> None: ...
