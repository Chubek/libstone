"""Static ACPI table emission."""

from __future__ import annotations

from dataclasses import dataclass
import struct


@dataclass(frozen=True, slots=True)
class AcpiTable:
    """A raw ACPI table blob."""

    signature: str
    payload: bytes


def build_acpi_tables() -> dict[str, AcpiTable]:
    """Return deterministic, checksum-valid minimal ACPI tables."""

    def checksum(blob: bytes) -> int:
        return (-sum(blob)) & 0xFF

    def table(signature: str, body: bytes, revision: int = 1) -> bytes:
        header = struct.pack("<4sIBB6s8sIII", signature.encode(
            "ascii"), 36 + len(body), revision, 0, b"DOMVMM", b"DOMLINT ", 1, 1, 1)
        blob = bytearray(header + body)
        blob[9] = checksum(blob)
        return bytes(blob)

    dsdt = table("DSDT", b"\x00")
    fadt = table("FACP", b"\x00" * 4)
    madt = table("APIC", b"\x00" * 8)
    xsdt = table("XSDT", b"")
    rsdp = bytearray(36)
    rsdp[0:8] = b"RSD PTR "
    rsdp[9:15] = b"DOMLNT"
    rsdp[15] = 2
    rsdp[20:24] = struct.pack("<I", 36)
    rsdp[24:32] = struct.pack("<Q", 0)
    rsdp[8] = checksum(rsdp[:20])
    rsdp[32] = checksum(rsdp)

    return {
        "RSDP": AcpiTable("RSDP", bytes(rsdp)),
        "XSDT": AcpiTable("XSDT", xsdt),
        "FADT": AcpiTable("FACP", fadt),
        "MADT": AcpiTable("APIC", madt),
        "DSDT": AcpiTable("DSDT", dsdt),
    }
