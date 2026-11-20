from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class MpTable:
    payload: bytes

def build_mptable() -> MpTable: ...
