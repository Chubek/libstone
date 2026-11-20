from dataclasses import dataclass

from domainlet.vmm.core.kvm.bindings import kvm_fpu, kvm_regs, kvm_sregs

@dataclass(frozen=True, slots=True)
class VcpuState:
    regs: kvm_regs
    sregs: kvm_sregs
    fpu: kvm_fpu | None = ...

@dataclass
class VcpuSnapshot:
    regs: kvm_regs
    sregs: kvm_sregs
    fpu: kvm_fpu | None = ...
