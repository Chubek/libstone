"""Boot-time helpers."""

from __future__ import annotations

from domainlet.vmm.boot.boot_params import BOOT_PARAMS_ADDRESS, BootParams, E820Entry
from domainlet.vmm.boot.loader import BootArtifacts, LinuxKernelLoader
from domainlet.vmm.boot.vcpu_init import configure_vcpu, initial_regs, initial_sregs

__all__ = [
    "BOOT_PARAMS_ADDRESS",
    "BootArtifacts",
    "BootParams",
    "E820Entry",
    "LinuxKernelLoader",
    "configure_vcpu",
    "initial_regs",
    "initial_sregs",
]
