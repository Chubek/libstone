from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class DeviceTree:
    blob: bytes

def build_fdt() -> DeviceTree: ...
