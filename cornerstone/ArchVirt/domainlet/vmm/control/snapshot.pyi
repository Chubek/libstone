from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class SnapshotEnvelope:
    version: int
    state: object

@dataclass
class SnapshotManager:
    version: int = ...
    _items: dict[str, Snapshottable]
    def register(self, name: str, item: Snapshottable) -> None: ...
    def save(self) -> SnapshotEnvelope: ...
    def restore(self, envelope: SnapshotEnvelope) -> None: ...
