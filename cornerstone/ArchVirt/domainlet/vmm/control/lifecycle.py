"""Clean shutdown ordering for VMM components."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class VmmLifecycle:
    """Reverse-order teardown for an assembled VMM."""

    vcpus: list[object] = field(default_factory=list)
    closeables: list[object] = field(default_factory=list)
    _closed: bool = field(default=False, init=False, repr=False)

    def shutdown(self) -> None:
        """Stop vCPUs first, then close resources in reverse order."""

        for vcpu in self.vcpus:
            if hasattr(vcpu, "stop"):
                vcpu.stop()
        for vcpu in self.vcpus:
            if hasattr(vcpu, "join"):
                vcpu.join(1.0)
        for item in reversed(self.closeables):
            if hasattr(item, "close"):
                item.close()
        self._closed = True

    def close(self) -> None:
        """Alias for shutdown."""

        self.shutdown()
        if self._closed:
            return
