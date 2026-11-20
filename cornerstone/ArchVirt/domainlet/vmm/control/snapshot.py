"""Snapshot save/restore helpers."""

from __future__ import annotations

from dataclasses import dataclass, field

from domainlet.vmm.seams import Snapshottable


@dataclass(frozen=True, slots=True)
class SnapshotEnvelope:
    """Versioned snapshot payload."""

    version: int
    state: object


@dataclass(slots=True)
class SnapshotManager:
    """Collect and restore snapshottable state."""

    version: int = 1
    _items: dict[str, Snapshottable] = field(default_factory=dict)

    def register(self, name: str, item: Snapshottable) -> None:
        self._items[name] = item

    def save(self) -> SnapshotEnvelope:
        return SnapshotEnvelope(self.version, {name: item.save_state() for name, item in self._items.items()})

    def restore(self, envelope: SnapshotEnvelope) -> None:
        if envelope.version != self.version:
            raise ValueError("snapshot version mismatch")
        state = envelope.state
        if not isinstance(state, dict):
            raise TypeError("invalid snapshot payload")
        for name, item in self._items.items():
            if name in state:
                item.restore_state(state[name])
