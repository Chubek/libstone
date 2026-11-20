"""Static MP table helpers."""

from __future__ import annotations

from dataclasses import dataclass
import struct


@dataclass(frozen=True, slots=True)
class MpTable:
    """A minimal MP table blob."""

    payload: bytes


def build_mptable() -> MpTable:
    """Return a checksum-valid minimal MP floating pointer structure."""

    blob = bytearray(b"_MP_" + struct.pack("<I", 0) +
                     bytes((1, 4, 0, 0, 0)) + b"\x00" * 4)
    blob[4:8] = struct.pack("<I", 0)
    blob[10] = (-sum(blob)) & 0xFF
    return MpTable(bytes(blob))
