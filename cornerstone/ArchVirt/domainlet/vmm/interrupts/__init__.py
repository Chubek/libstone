"""Interrupt controllers and sinks."""

from __future__ import annotations

from domainlet.vmm.interrupts.irqchip import IrqChip, KvmIrqChip
from domainlet.vmm.interrupts.irqfd import IrqFdSink

__all__ = ["IrqChip", "IrqFdSink", "KvmIrqChip"]
