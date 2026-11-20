"""Guest memory mapping and registration helpers."""

from __future__ import annotations

import ctypes
import mmap
import os
from dataclasses import dataclass, field
from typing import Final

from domainlet.vmm.core.kvm.bindings import kvm_userspace_memory_region
from domainlet.vmm.errors import MemoryError

PAGE_SIZE: Final[int] = os.sysconf("SC_PAGE_SIZE")


def align_up(value: int, align: int = PAGE_SIZE) -> int:
    """Round *value* up to the next *align* boundary."""

    return (value + align - 1) // align * align


@dataclass(frozen=True, slots=True)
class GuestAddress:
    """A guest physical address."""

    value: int


@dataclass(frozen=True, slots=True)
class MemoryRegion:
    """A contiguous guest memory slot."""

    slot: int
    guest_phys_addr: int
    memory_size: int
    flags: int = 0

    def __post_init__(self) -> None:
        if self.memory_size <= 0:
            raise ValueError("memory size must be positive")
        if self.guest_phys_addr < 0:
            raise ValueError("guest address must be non-negative")


@dataclass(slots=True)
class GuestMemoryMmap:
    """Anonymous guest RAM backed by a Python mmap object."""

    size: int
    regions: tuple[MemoryRegion, ...] = ()
    _mmap: mmap.mmap = field(init=False, repr=False)
    _base: int = field(init=False, repr=False)

    def __post_init__(self) -> None:
        if self.size <= 0:
            raise MemoryError("guest memory size must be positive")
        self.size = align_up(self.size)
        try:
            self._mmap = mmap.mmap(-1, self.size, access=mmap.ACCESS_WRITE)
        except OSError as exc:
            raise MemoryError(
                f"unable to allocate guest memory: {exc}") from exc
        self._base = ctypes.addressof(ctypes.c_char.from_buffer(self._mmap))
        if not self.regions:
            self.regions = (MemoryRegion(0, 0, self.size),)
        for region in self.regions:
            if region.guest_phys_addr + region.memory_size > self.size:
                self._mmap.close()
                raise MemoryError(
                    f"memory region {region.slot} exceeds guest memory size"
                )

    def close(self) -> None:
        """Release the backing mmap."""

        if getattr(self, "_mmap", None) is not None:
            self._mmap.close()

    def translate(self, addr: int) -> memoryview:
        """Return a view into guest RAM."""

        if addr < 0 or addr >= self.size:
            raise MemoryError(f"guest address out of bounds: {addr:#x}")
        return memoryview(self._mmap)[addr:]

    def read(self, addr: int, size: int) -> bytes:
        """Read bytes from guest RAM."""

        if addr < 0 or size < 0 or addr + size > self.size:
            raise MemoryError(f"guest read out of bounds: {addr:#x}+{size}")
        return bytes(self._mmap[addr: addr + size])

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        """Write bytes to guest RAM."""

        payload = bytes(data)
        if addr < 0 or addr + len(payload) > self.size:
            raise MemoryError(
                f"guest write out of bounds: {addr:#x}+{len(payload)}")
        self._mmap.seek(addr)
        self._mmap.write(payload)
        return len(payload)

    def register_with_vm(self, vm: object) -> None:
        """Install every configured region into a KVM VM."""

        if not hasattr(vm, "set_user_memory_region"):
            raise MemoryError(
                "VM object does not expose set_user_memory_region()")
        for region in self.regions:
            descriptor = kvm_userspace_memory_region()
            descriptor.slot = region.slot
            descriptor.flags = region.flags
            descriptor.guest_phys_addr = region.guest_phys_addr
            descriptor.memory_size = region.memory_size
            descriptor.userspace_addr = self._base + region.guest_phys_addr
            vm.set_user_memory_region(descriptor)
