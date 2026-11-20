"""Bindlets for the :mod:`domainlet.vmm.interrupts` package."""

from __future__ import annotations

from domainlet.bindlets.vmm_bindlets import _VmmPackageBindlet


class VmmInterruptsBindlet(_VmmPackageBindlet):
    """Bindlet for the VMM interrupt modules."""

    bindlet_name = "vmm.interrupts"
    package_name = "domainlet.vmm.interrupts"
    description = "Interrupt controller, irqfd, MSI routing, and timer helpers."


__all__ = ["VmmInterruptsBindlet"]
