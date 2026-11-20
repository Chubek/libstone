"""In-kernel irqchip wrapper."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol

from domainlet.vmm.core.kvm.bindings import KVM_IRQ_LINE, kvm_irq_level, ioctl


class IrqChip(Protocol):
    """Interrupt controller seam."""

    def signal(self, vector: int = 0) -> None: ...


@dataclass(slots=True)
class KvmIrqChip:
    """Delegate interrupt routing to KVM."""

    vm: object

    def __post_init__(self) -> None:
        if hasattr(self.vm, "create_irqchip"):
            self.vm.create_irqchip()

    def signal(self, vector: int = 0) -> None:
        level = kvm_irq_level()
        level.irq = vector
        level.level = 1
        ioctl(self.vm.fd, KVM_IRQ_LINE, level)
