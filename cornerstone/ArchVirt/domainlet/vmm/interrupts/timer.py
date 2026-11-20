"""Timer helpers."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True)
class KvmClock:
    """Placeholder kvmclock setup."""

    enabled: bool = True
