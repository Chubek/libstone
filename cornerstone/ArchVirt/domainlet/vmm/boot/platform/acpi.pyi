from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class AcpiTable:
    signature: str
    payload: bytes

def build_acpi_tables() -> dict[str, AcpiTable]: ...
