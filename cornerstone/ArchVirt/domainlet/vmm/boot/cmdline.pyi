def build_cmdline(*parts: str) -> str: ...

from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class Cmdline:
    value: str
