"""Core VMM building blocks."""

from __future__ import annotations

from domainlet.vmm.core.allocator import AddressAllocator, AddressRange
from domainlet.vmm.core.exits import ExitDispatcher, VmExit, VmExitReason
from domainlet.vmm.core.guest_memory import GuestAddress, GuestMemoryMmap, MemoryRegion

__all__ = [
    "AddressAllocator",
    "AddressRange",
    "ExitDispatcher",
    "GuestAddress",
    "GuestMemoryMmap",
    "MemoryRegion",
    "VmExit",
    "VmExitReason",
]
