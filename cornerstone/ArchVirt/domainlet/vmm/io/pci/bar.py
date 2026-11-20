"""PCI BAR helpers."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class PciBar:
    """A PCI base-address register."""

    index: int
    size: int
    address: int = 0
    is_io: bool = False
    prefetchable: bool = False
