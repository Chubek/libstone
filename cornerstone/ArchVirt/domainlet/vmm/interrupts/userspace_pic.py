"""Userspace PIC fallback."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(slots=True)
class UserspacePic:
    """Small edge-triggered PIC fallback with pending-vector state."""

    pending: list[int] = field(default_factory=list)

    def signal(self, vector: int = 0) -> None:
        if not 0 <= vector <= 255:
            raise ValueError("interrupt vector must be in range 0..255")
        self.pending.append(vector)

    def acknowledge(self) -> int | None:
        """Return and remove the oldest pending vector."""

        return self.pending.pop(0) if self.pending else None
