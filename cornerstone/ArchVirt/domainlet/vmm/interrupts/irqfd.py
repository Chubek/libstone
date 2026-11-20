"""eventfd-backed interrupt sinks."""

from __future__ import annotations

from dataclasses import dataclass

from domainlet.vmm.io.eventfd import EventFd


@dataclass(slots=True)
class IrqFdSink:
    """Interrupt sink backed by an eventfd."""

    eventfd: EventFd

    def signal(self, vector: int = 0) -> None:
        self.eventfd.write(1)
