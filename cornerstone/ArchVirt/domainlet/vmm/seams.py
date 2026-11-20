"""Typed seams shared by the VMM domainlets."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol, runtime_checkable

FileDescriptor = int


@runtime_checkable
class GuestMemoryLike(Protocol):
    """Guest memory abstraction used by device domainlets."""

    size: int

    def read(self, addr: int, size: int) -> bytes: ...

    def write(self, addr: int, data: bytes |
              bytearray | memoryview) -> int: ...

    def translate(self, addr: int) -> memoryview: ...


@runtime_checkable
class BusDevice(Protocol):
    """Addressable device attached to the MMIO or PIO bus."""

    base: int
    size: int

    def read(self, addr: int, size: int) -> bytes: ...

    def write(self, addr: int, data: bytes |
              bytearray | memoryview) -> int: ...


@runtime_checkable
class InterruptSink(Protocol):
    """Recipient for interrupt notifications."""

    def signal(self, vector: int = 0) -> None: ...


@runtime_checkable
class Snapshottable(Protocol):
    """State that can be serialized and restored."""

    def save_state(self) -> object: ...

    def restore_state(self, state: object) -> None: ...


@runtime_checkable
class EventSubscriber(Protocol):
    """epoll subscriber contract."""

    def fileno(self) -> int: ...

    def on_event(self, events: int) -> None: ...


@runtime_checkable
class ResourceAllocatorLike(Protocol):
    """Allocator for guest physical and I/O address spaces."""

    def allocate_gpa(self, size: int, align: int = ...) -> int: ...

    def allocate_mmio(self, size: int, align: int = ...) -> int: ...

    def allocate_pio(self, size: int, align: int = ...) -> int: ...

    def reserve(self, start: int, size: int) -> None: ...
