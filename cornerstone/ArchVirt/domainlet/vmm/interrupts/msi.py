"""MSI routing helpers."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class MsiRouteTable:
    """A simple MSI route registry."""

    routes: dict[int, int] = field(default_factory=dict)

    def add(self, vector: int, gsi: int) -> None:
        self.routes[vector] = gsi
