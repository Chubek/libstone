"""Initial vCPU state for Linux direct boot."""

from __future__ import annotations

from dataclasses import dataclass

from domainlet.vmm.core.kvm.bindings import KVM_SET_REGS, KVM_SET_SREGS, kvm_regs, kvm_sregs, ioctl
from domainlet.vmm.boot.boot_params import BOOT_PARAMS_ADDRESS

CR0_PE = 1 << 0
CR0_PG = 1 << 31
CR4_PAE = 1 << 5
EFER_LME = 1 << 8
RFLAGS_RESERVED = 1 << 1


def initial_regs(entry_point: int, stack_pointer: int, boot_params_address: int = BOOT_PARAMS_ADDRESS) -> kvm_regs:
    """Build initial general-purpose register state."""

    regs = kvm_regs()
    regs.rip = entry_point
    regs.rsp = stack_pointer
    regs.rsi = boot_params_address
    regs.rflags = RFLAGS_RESERVED
    return regs


def initial_sregs() -> kvm_sregs:
    """Build a minimal protected-mode register set."""

    sregs = kvm_sregs()
    sregs.cr0 = CR0_PE | CR0_PG
    sregs.cr4 = CR4_PAE
    sregs.efer = EFER_LME
    sregs.cs.base = 0
    sregs.cs.selector = 0
    sregs.cs.present = 1
    sregs.cs.l = 1
    sregs.cs.db = 0
    sregs.cs.s = 1
    sregs.cs.type = 0xB
    return sregs


def configure_vcpu(vcpu, entry_point: int, stack_pointer: int, boot_params_address: int = BOOT_PARAMS_ADDRESS) -> None:
    """Apply the initial boot state to a vCPU object."""

    ioctl(vcpu.fd, KVM_SET_REGS, initial_regs(
        entry_point, stack_pointer, boot_params_address))
    ioctl(vcpu.fd, KVM_SET_SREGS, initial_sregs())
