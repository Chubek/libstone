"""Address-space allocator for guest resources."""

from __future__ import annotations

from dataclasses import dataclass, field

from domainlet.vmm.errors import AllocatorError
from domainlet.vmm.seams import ResourceAllocatorLike


@dataclass(frozen=True, slots=True)
class AddressRange:
    """A reserved inclusive address range."""

    start: int
    end: int


@dataclass(slots=True)
class AddressAllocator(ResourceAllocatorLike):
    """Simple first-fit allocator for GPA, MMIO, and PIO."""

    gpa_base: int = 0x1000
    mmio_base: int = 0x1000_0000
    pio_base: int = 0x1000
    _ranges: list[AddressRange] = field(default_factory=list)

    def reserve(self, start: int, size: int) -> None:
        """Reserve a raw address range."""

        if start < 0:
            raise AllocatorError("address must be non-negative")
        if size <= 0:
            raise AllocatorError("reservation size must be positive")
        end = start + size
        if end <= start:
            raise AllocatorError("reservation overflows address space")
        for item in self._ranges:
            if start < item.end and item.start < end:
                raise AllocatorError(
                    f"address range {start:#x}-{end:#x} overlaps "
                    f"{item.start:#x}-{item.end:#x}"
                )
        self._ranges.append(AddressRange(start, end))
        self._ranges.sort(key=lambda item: item.start)

    def _allocate(self, base: int, size: int, align: int) -> int:
        if size <= 0:
            raise AllocatorError("allocation size must be positive")
        if base < 0:
            raise AllocatorError("allocator base must be non-negative")
        if align <= 0 or align & (align - 1):
            raise AllocatorError("alignment must be a positive power of two")
        cursor = base
        for item in self._ranges:
            candidate = ((cursor + align - 1) // align) * align
            if candidate + size <= item.start:
                self.reserve(candidate, size)
                return candidate
            cursor = max(cursor, item.end)
        candidate = ((cursor + align - 1) // align) * align
        self.reserve(candidate, size)
        return candidate

    def allocate_gpa(self, size: int, align: int = 0x1000) -> int:
        """Allocate guest physical memory."""

        return self._allocate(self.gpa_base, size, align)

    def allocate_mmio(self, size: int, align: int = 0x1000) -> int:
        """Allocate MMIO space."""

        return self._allocate(self.mmio_base, size, align)

    def allocate_pio(self, size: int, align: int = 0x10) -> int:
        """Allocate port I/O space."""

        return self._allocate(self.pio_base, size, align)
