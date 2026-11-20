"""Bindlets for the :mod:`domainlet.vmm.boot` package."""

from __future__ import annotations

from domainlet.bindlets.vmm_bindlets import _VmmPackageBindlet


class VmmBootBindlet(_VmmPackageBindlet):
    """Bindlet for the VMM boot modules."""

    bindlet_name = "vmm.boot"
    package_name = "domainlet.vmm.boot"
    description = "Boot loader, boot parameters, kernel command line, and platform tables."


__all__ = ["VmmBootBindlet"]
