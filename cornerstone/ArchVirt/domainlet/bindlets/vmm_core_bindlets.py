"""Bindlets for the :mod:`domainlet.vmm.core` package."""

from __future__ import annotations

from domainlet.bindlets.vmm_bindlets import _VmmPackageBindlet


class VmmCoreBindlet(_VmmPackageBindlet):
    """Bindlet for the core VMM modules."""

    bindlet_name = "vmm.core"
    package_name = "domainlet.vmm.core"
    description = "Core VMM modules: KVM bindings, memory, allocator, exits, and vCPU state."


__all__ = ["VmmCoreBindlet"]
