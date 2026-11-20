"""Interrupt adapter for VirtIO domainlets."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True)
class VirtioIrqAdapter:
    """Translate a VirtIO notifier into an interrupt sink."""

    sink: object

    def signal(self, vector: int = 0) -> None:
        if hasattr(self.sink, "signal"):
            self.sink.signal(vector)
