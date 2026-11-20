from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import pytest

import domainlet.vmm.control.builder as builder
from domainlet.vmm.control.config import VmConfig
from domainlet.vmm.tests.fakes import FakeDevice, FakeGuestMemory, FakeIrq


@dataclass
class _FakeHypervisor:
    fd: int = 1

    def __init__(self) -> None:
        self.fd = 1

    def create_vm(self, vm_type: int = 0):
        return self

    def create_vcpu(self, vcpu_id: int) -> int:
        return 3

    def create_irqchip(self) -> None:
        return None

    def set_user_memory_region(self, region) -> None:
        return None

    def vcpu_mmap_size(self) -> int:
        return 4096

    def close(self) -> None:
        return None


@dataclass
class _FakeVcpu:
    fd: int
    mmap_size: int
    bus: object

    def stop(self) -> None:
        return None

    def join(self, timeout: float | None = None) -> None:
        return None


def test_builder_can_assemble_with_fakes(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    kernel = tmp_path / "kernel.bin"
    kernel.write_bytes(b"\x90" * 64)
    monkeypatch.setattr(builder, "Hypervisor", _FakeHypervisor)
    monkeypatch.setattr(builder, "GuestMemoryMmap", FakeGuestMemory)
    monkeypatch.setattr(builder, "Vcpu", _FakeVcpu)
    monkeypatch.setattr(builder, "configure_vcpu",
                        lambda *args, **kwargs: None)
    monkeypatch.setattr(builder, "KvmIrqChip", lambda vm: FakeIrq())
    monkeypatch.setattr(builder, "SerialPort", lambda: FakeDevice())
    assembly = builder.build_vmm(VmConfig(kernel, memory_size=4096))
    assert assembly.lifecycle is not None
