"""Platform tables for direct boot."""

from __future__ import annotations

from domainlet.vmm.boot.platform.acpi import build_acpi_tables
from domainlet.vmm.boot.platform.mptable import build_mptable

__all__ = ["build_acpi_tables", "build_mptable"]
