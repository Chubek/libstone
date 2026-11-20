from __future__ import annotations

from domainlet.vmm.boot.platform.acpi import build_acpi_tables


def test_acpi_tables_exist() -> None:
    tables = build_acpi_tables()
    assert "RSDP" in tables and "XSDT" in tables
