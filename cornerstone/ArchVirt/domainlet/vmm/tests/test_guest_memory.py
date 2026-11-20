from __future__ import annotations

import pytest

from domainlet.vmm.core.guest_memory import GuestMemoryMmap, MemoryError
from domainlet.vmm.tests.fakes import FakeGuestMemory


def test_fake_guest_memory_roundtrip() -> None:
    mem = FakeGuestMemory(16)
    assert mem.write(4, b"ab") == 2
    assert mem.read(4, 2) == b"ab"


def test_guest_memory_mmap_bounds() -> None:
    mem = GuestMemoryMmap(4096)
    try:
        mem.write(0, b"abc")
        assert mem.read(0, 3) == b"abc"
        with pytest.raises(MemoryError):
            mem.read(4096, 1)
    finally:
        mem.close()
