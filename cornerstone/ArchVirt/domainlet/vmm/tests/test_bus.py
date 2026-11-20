from __future__ import annotations

import pytest

from domainlet.vmm.core.guest_memory import MemoryError
from domainlet.vmm.errors import BusError
from domainlet.vmm.io.bus import Bus
from domainlet.vmm.tests.fakes import FakeDevice


def test_bus_routes_reads_and_writes() -> None:
    bus = Bus()
    dev = FakeDevice(base=0x1000, size=0x100)
    bus.register(dev.base, dev.size, dev)
    assert bus.write(0x1010, b"hi") == 2
    assert bus.read(0x1010, 2) == b"\x00\x00"


def test_bus_rejects_unknown_address() -> None:
    with pytest.raises(BusError):
        Bus().read(0xdead, 1)
