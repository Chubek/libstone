from __future__ import annotations

from domainlet.vmm.core.allocator import AddressAllocator


def test_allocator_allocates_non_overlapping_ranges() -> None:
    allocator = AddressAllocator()
    first = allocator.allocate_gpa(0x1000)
    second = allocator.allocate_gpa(0x1000)
    assert second >= first + 0x1000
