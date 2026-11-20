from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class MsixEntry:
    address: int
    data: int
    vector_control: int = ...

@dataclass
class MsixTable:
    entries: list[MsixEntry]
