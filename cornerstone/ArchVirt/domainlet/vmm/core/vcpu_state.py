"""vCPU register snapshots."""

from __future__ import annotations

from dataclasses import dataclass, field

from domainlet.vmm.core.kvm.bindings import kvm_fpu, kvm_regs, kvm_sregs


@dataclass(frozen=True, slots=True)
class VcpuState:
    """Full x86 vCPU snapshot."""

    regs: kvm_regs
    sregs: kvm_sregs
    fpu: kvm_fpu | None = None


@dataclass(slots=True)
class VcpuSnapshot:
    """Mutable snapshot container for save/restore flows."""

    regs: kvm_regs = field(default_factory=kvm_regs)
    sregs: kvm_sregs = field(default_factory=kvm_sregs)
    fpu: kvm_fpu | None = None
