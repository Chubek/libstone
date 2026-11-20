"""Test doubles for VMM domainlets."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Callable

from domainlet.vmm.seams import BusDevice, EventSubscriber, InterruptSink, Snapshottable


@dataclass(slots=True)
class FakeGuestMemory(Snapshottable):
    """A bytearray-backed guest memory double."""

    size: int
    data: bytearray = field(init=False, repr=False)

    def __post_init__(self) -> None:
        self.data = bytearray(self.size)

    def read(self, addr: int, size: int) -> bytes:
        return bytes(self.data[addr: addr + size])

    def write(self, addr: int, payload: bytes | bytearray | memoryview) -> int:
        blob = bytes(payload)
        self.data[addr: addr + len(blob)] = blob
        return len(blob)

    def translate(self, addr: int) -> memoryview:
        return memoryview(self.data)[addr:]

    def register_with_vm(self, vm: object) -> None:
        """Mirror the real memory mapper's VM registration hook."""

        return None

    def save_state(self) -> object:
        return bytes(self.data)

    def restore_state(self, state: object) -> None:
        self.data[:] = bytes(state)  # type: ignore[arg-type]


@dataclass(slots=True)
class FakeDevice(BusDevice):
    """A simple bus device double."""

    base: int = 0
    size: int = 0x100
    reads: list[tuple[int, int]] = field(default_factory=list, repr=False)
    writes: list[tuple[int, bytes]] = field(default_factory=list, repr=False)

    def read(self, addr: int, size: int) -> bytes:
        self.reads.append((addr, size))
        return bytes(size)

    def write(self, addr: int, data: bytes | bytearray | memoryview) -> int:
        blob = bytes(data)
        self.writes.append((addr, blob))
        return len(blob)


@dataclass(slots=True)
class FakeIrq(InterruptSink):
    """A captured interrupt sink."""

    vectors: list[int] = field(default_factory=list)

    def signal(self, vector: int = 0) -> None:
        self.vectors.append(vector)


@dataclass(slots=True)
class FakeSubscriber(EventSubscriber):
    """A pipe-backed epoll subscriber."""

    callback: Callable[[int], None]
    _r: int = field(init=False, repr=False)
    _w: int = field(init=False, repr=False)
    events: list[int] = field(default_factory=list)

    def __post_init__(self) -> None:
        self._r, self._w = os.pipe()

    def fileno(self) -> int:
        return self._r

    def trigger(self) -> None:
        os.write(self._w, b"x")

    def on_event(self, events: int) -> None:
        self.events.append(events)
        os.read(self._r, 1)
        self.callback(events)

    def close(self) -> None:
        os.close(self._r)
        os.close(self._w)
