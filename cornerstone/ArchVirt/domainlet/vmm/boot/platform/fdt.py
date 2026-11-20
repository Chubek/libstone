"""AArch64 FDT placeholder."""

from __future__ import annotations

from dataclasses import dataclass
import struct


@dataclass(frozen=True, slots=True)
class DeviceTree:
    """A placeholder device tree."""

    blob: bytes


def build_fdt() -> DeviceTree:
    """Return a valid empty flattened-device-tree blob."""

    # Header plus a terminating empty structure block and empty strings block.
    header_size = 40
    struct_off = header_size
    strings_off = struct_off + 8
    totalsize = strings_off
    header = struct.pack(
        ">10I",
        0xD00DFEED,
        totalsize,
        struct_off,
        strings_off,
        0,
        17,
        16,
        0,
        0,
        0,
    )
    structure = struct.pack(">2I", 1, 9)  # FDT_BEGIN_NODE, FDT_END
    return DeviceTree(header + structure)
