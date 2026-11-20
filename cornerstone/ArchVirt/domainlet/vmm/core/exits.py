"""KVM exit decoding helpers."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum
from typing import Callable

from domainlet.vmm.core.kvm.bindings import (
    KVM_EXIT_HLT,
    KVM_EXIT_INTERNAL_ERROR,
    KVM_EXIT_IO,
    KVM_EXIT_IOAPIC_EOI,
    KVM_EXIT_MMIO,
    KVM_EXIT_NOTIFY,
    KVM_EXIT_SHUTDOWN,
)


class VmExitReason(IntEnum):
    """Human-readable KVM exit reasons."""

    HLT = KVM_EXIT_HLT
    IO = KVM_EXIT_IO
    MMIO = KVM_EXIT_MMIO
    SHUTDOWN = KVM_EXIT_SHUTDOWN
    INTERNAL_ERROR = KVM_EXIT_INTERNAL_ERROR
    IOAPIC_EOI = KVM_EXIT_IOAPIC_EOI
    NOTIFY = KVM_EXIT_NOTIFY


@dataclass(frozen=True, slots=True)
class VmExit:
    """Decoded KVM exit."""

    reason: VmExitReason
    detail: object | None = None


@dataclass(frozen=True, slots=True)
class MmioExit(VmExit):
    """MMIO access from the guest."""

    address: int = 0
    data: bytes = b""
    is_write: bool = False


@dataclass(frozen=True, slots=True)
class IoExit(VmExit):
    """PIO access from the guest."""

    port: int = 0
    data: bytes = b""
    is_write: bool = False


@dataclass(slots=True)
class ExitDispatcher:
    """Dispatch decoded exits to handler callables."""

    handlers: dict[VmExitReason, Callable[[VmExit],
                                          object | None]] = field(default_factory=dict)

    def dispatch(self, vm_exit: VmExit) -> object | None:
        if not isinstance(vm_exit, VmExit):
            raise TypeError("dispatch expects a VmExit instance")
        handler = self.handlers.get(vm_exit.reason)
        return None if handler is None else handler(vm_exit)
