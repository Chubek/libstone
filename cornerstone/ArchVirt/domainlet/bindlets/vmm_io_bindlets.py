"""Bindlets for the :mod:`domainlet.vmm.io` package."""

from __future__ import annotations

from domainlet.bindlets.vmm_bindlets import _VmmPackageBindlet


class VmmIoBindlet(_VmmPackageBindlet):
    """Bindlet for the VMM I/O modules."""

    bindlet_name = "vmm.io"
    package_name = "domainlet.vmm.io"
    description = "Device buses, event loop, eventfd helpers, PCI, and backends."


__all__ = ["VmmIoBindlet"]
