from dataclasses import dataclass
from domainlet.vmm.io.eventfd import EventFd

@dataclass
class IrqFdSink:
    eventfd: EventFd
    def signal(self, vector: int = ...) -> None: ...
