"""Adapters bridging VirtIO domainlets to VMM seams."""

from __future__ import annotations

from domainlet.vmm.virtio_adapter.bus_adapter import VirtioBusAdapter
from domainlet.vmm.virtio_adapter.irq_adapter import VirtioIrqAdapter
from domainlet.vmm.virtio_adapter.mem_adapter import VirtioMemoryAdapter

__all__ = ["VirtioBusAdapter", "VirtioIrqAdapter", "VirtioMemoryAdapter"]
