"""Bindlets for the :mod:`domainlet.vmm.virtio_adapter` package."""

from __future__ import annotations

from domainlet.bindlets.vmm_bindlets import _VmmPackageBindlet


class VmmVirtioAdapterBindlet(_VmmPackageBindlet):
    """Bindlet for the VirtIO adapter modules."""

    bindlet_name = "vmm.virtio_adapter"
    package_name = "domainlet.vmm.virtio_adapter"
    description = "Adapters that bridge internal VirtIO domainlets to VMM seams."


__all__ = ["VmmVirtioAdapterBindlet"]
