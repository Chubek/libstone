"""MSI-X helpers."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True, slots=True)
class MsixEntry:
    """A single MSI-X table entry."""

    address: int
    data: int
    vector_control: int = 0


@dataclass(slots=True)
class MsixTable:
    """MSI-X state for a PCI function."""

    entries: list[MsixEntry] = field(default_factory=list)
