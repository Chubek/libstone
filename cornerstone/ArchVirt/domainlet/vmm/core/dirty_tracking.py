"""Dirty page logging helpers."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class DirtyBitmap:
    """A raw dirty-log bitmap."""

    slot: int
    bitmap: bytes
