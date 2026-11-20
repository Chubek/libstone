"""Bindlets for the :mod:`domainlet.vmm.control` package."""

from __future__ import annotations

from domainlet.bindlets.vmm_bindlets import _VmmPackageBindlet


class VmmControlBindlet(_VmmPackageBindlet):
    """Bindlet for the VMM control modules."""

    bindlet_name = "vmm.control"
    package_name = "domainlet.vmm.control"
    description = "Builder, lifecycle, CLI, API, snapshot, sandbox, and VM config modules."


__all__ = ["VmmControlBindlet"]
