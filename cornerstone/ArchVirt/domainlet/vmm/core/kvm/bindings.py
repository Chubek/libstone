"""ctypes surface for the Linux KVM userspace ABI."""

from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass
from typing import Final

KVMIO: Final[int] = 0xAE
KVM_API_VERSION: Final[int] = 12


def _ioc(dir_bits: int, type_bits: int, nr: int, size: int) -> int:
    return ((dir_bits << 30) | (type_bits << 8) | nr | (size << 16))


def _io(nr: int) -> int:
    return _ioc(0, KVMIO, nr, 0)


def _ior(nr: int, size: int) -> int:
    return _ioc(2, KVMIO, nr, size)


def _iow(nr: int, size: int) -> int:
    return _ioc(1, KVMIO, nr, size)


def _iowr(nr: int, size: int) -> int:
    return _ioc(3, KVMIO, nr, size)


class KvmIoctlError(OSError):
    """Raised when a KVM ioctl fails."""


class kvm_userspace_memory_region(ctypes.Structure):
    """Legacy userspace memory slot definition."""

    _fields_ = [
        ("slot", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("guest_phys_addr", ctypes.c_uint64),
        ("memory_size", ctypes.c_uint64),
        ("userspace_addr", ctypes.c_uint64),
    ]


class kvm_userspace_memory_region2(ctypes.Structure):
    """Extended userspace memory slot definition."""

    _fields_ = [
        ("slot", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("guest_phys_addr", ctypes.c_uint64),
        ("memory_size", ctypes.c_uint64),
        ("userspace_addr", ctypes.c_uint64),
        ("guest_memfd_offset", ctypes.c_uint64),
        ("guest_memfd", ctypes.c_uint32),
        ("pad1", ctypes.c_uint32),
        ("pad2", ctypes.c_uint64 * 14),
    ]


class kvm_regs(ctypes.Structure):
    """x86 general purpose register state."""

    _fields_ = [
        ("rax", ctypes.c_uint64),
        ("rbx", ctypes.c_uint64),
        ("rcx", ctypes.c_uint64),
        ("rdx", ctypes.c_uint64),
        ("rsi", ctypes.c_uint64),
        ("rdi", ctypes.c_uint64),
        ("rsp", ctypes.c_uint64),
        ("rbp", ctypes.c_uint64),
        ("r8", ctypes.c_uint64),
        ("r9", ctypes.c_uint64),
        ("r10", ctypes.c_uint64),
        ("r11", ctypes.c_uint64),
        ("r12", ctypes.c_uint64),
        ("r13", ctypes.c_uint64),
        ("r14", ctypes.c_uint64),
        ("r15", ctypes.c_uint64),
        ("rip", ctypes.c_uint64),
        ("rflags", ctypes.c_uint64),
    ]


class kvm_segment(ctypes.Structure):
    """x86 segment descriptor."""

    _fields_ = [
        ("base", ctypes.c_uint64),
        ("limit", ctypes.c_uint32),
        ("selector", ctypes.c_uint16),
        ("type", ctypes.c_uint8),
        ("present", ctypes.c_uint8),
        ("dpl", ctypes.c_uint8),
        ("db", ctypes.c_uint8),
        ("s", ctypes.c_uint8),
        ("l", ctypes.c_uint8),
        ("g", ctypes.c_uint8),
        ("avl", ctypes.c_uint8),
        ("unusable", ctypes.c_uint8),
        ("padding", ctypes.c_uint8),
    ]


class kvm_dtable(ctypes.Structure):
    """x86 descriptor table register."""

    _fields_ = [("base", ctypes.c_uint64),
                ("limit", ctypes.c_uint16), ("padding", ctypes.c_uint16 * 3)]


class kvm_sregs(ctypes.Structure):
    """x86 special register state."""

    _fields_ = [
        ("cs", kvm_segment),
        ("ds", kvm_segment),
        ("es", kvm_segment),
        ("fs", kvm_segment),
        ("gs", kvm_segment),
        ("ss", kvm_segment),
        ("tr", kvm_segment),
        ("ldt", kvm_segment),
        ("gdt", kvm_dtable),
        ("idt", kvm_dtable),
        ("cr0", ctypes.c_uint64),
        ("cr2", ctypes.c_uint64),
        ("cr3", ctypes.c_uint64),
        ("cr4", ctypes.c_uint64),
        ("cr8", ctypes.c_uint64),
        ("efer", ctypes.c_uint64),
        ("apic_base", ctypes.c_uint64),
        ("interrupt_bitmap", ctypes.c_uint64 * 4),
    ]


class kvm_fpu(ctypes.Structure):
    """x87 and SSE state."""

    _fields_ = [
        ("fpr", (ctypes.c_uint8 * 16) * 8),
        ("fcw", ctypes.c_uint16),
        ("fsw", ctypes.c_uint16),
        ("ftwx", ctypes.c_uint8),
        ("pad1", ctypes.c_uint8),
        ("last_opcode", ctypes.c_uint16),
        ("last_ip", ctypes.c_uint64),
        ("last_dp", ctypes.c_uint64),
        ("xmm", (ctypes.c_uint8 * 16) * 16),
        ("mxcsr", ctypes.c_uint32),
        ("pad2", ctypes.c_uint32),
    ]


class kvm_cpuid_entry2(ctypes.Structure):
    """A CPUID leaf entry."""

    _fields_ = [
        ("function", ctypes.c_uint32),
        ("index", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("eax", ctypes.c_uint32),
        ("ebx", ctypes.c_uint32),
        ("ecx", ctypes.c_uint32),
        ("edx", ctypes.c_uint32),
        ("padding", ctypes.c_uint32 * 3),
    ]


class kvm_cpuid2(ctypes.Structure):
    """A CPUID entry list."""

    _fields_ = [("nent", ctypes.c_uint32), ("padding",
                                            ctypes.c_uint32), ("entries", kvm_cpuid_entry2 * 1)]


class kvm_irq_level(ctypes.Structure):
    """An IRQ line state request."""

    _fields_ = [("irq", ctypes.c_uint32), ("level", ctypes.c_uint32)]


class kvm_ioeventfd(ctypes.Structure):
    """ioeventfd registration request."""

    _fields_ = [
        ("datamatch", ctypes.c_uint64),
        ("addr", ctypes.c_uint64),
        ("len", ctypes.c_uint32),
        ("fd", ctypes.c_int32),
        ("flags", ctypes.c_uint32),
        ("pad", ctypes.c_uint8 * 36),
    ]


class _kvm_run_io(ctypes.Structure):
    _fields_ = [
        ("direction", ctypes.c_uint8),
        ("size", ctypes.c_uint8),
        ("port", ctypes.c_uint16),
        ("count", ctypes.c_uint32),
        ("data_offset", ctypes.c_uint64),
    ]


class _kvm_run_mmio(ctypes.Structure):
    _fields_ = [
        ("phys_addr", ctypes.c_uint64),
        ("data", ctypes.c_uint8 * 8),
        ("len", ctypes.c_uint32),
        ("is_write", ctypes.c_uint8),
        ("padding", ctypes.c_uint8 * 3),
    ]


class _kvm_run_union(ctypes.Union):
    _fields_ = [
        ("io", _kvm_run_io),
        ("mmio", _kvm_run_mmio),
        ("padding", ctypes.c_uint8 * 256),
    ]


class kvm_run(ctypes.Structure):
    """Shared vCPU run page."""

    _anonymous_ = ("u",)
    _fields_ = [
        ("request_interrupt_window", ctypes.c_uint8),
        ("immediate_exit", ctypes.c_uint8),
        ("padding1", ctypes.c_uint8 * 6),
        ("exit_reason", ctypes.c_uint32),
        ("ready_for_interrupt_injection", ctypes.c_uint8),
        ("if_flag", ctypes.c_uint8),
        ("flags", ctypes.c_uint16),
        ("cr8", ctypes.c_uint64),
        ("apic_base", ctypes.c_uint64),
        ("u", _kvm_run_union),
        ("kvm_valid_regs", ctypes.c_uint64),
        ("kvm_dirty_regs", ctypes.c_uint64),
        ("sync", ctypes.c_uint8 * 2048),
    ]


KVM_GET_API_VERSION: Final[int] = _io(0x00)
KVM_CREATE_VM: Final[int] = _io(0x01)
KVM_CHECK_EXTENSION: Final[int] = _io(0x03)
KVM_GET_VCPU_MMAP_SIZE: Final[int] = _io(0x04)
KVM_CREATE_VCPU: Final[int] = _io(0x41)
KVM_SET_USER_MEMORY_REGION: Final[int] = _iow(
    0x46, ctypes.sizeof(kvm_userspace_memory_region))
KVM_SET_USER_MEMORY_REGION2: Final[int] = _iow(
    0x49, ctypes.sizeof(kvm_userspace_memory_region2))
KVM_CREATE_IRQCHIP: Final[int] = _io(0x60)
KVM_IRQ_LINE: Final[int] = _iow(0x61, ctypes.sizeof(kvm_irq_level))
KVM_RUN: Final[int] = _io(0x80)
KVM_GET_REGS: Final[int] = _ior(0x81, ctypes.sizeof(kvm_regs))
KVM_SET_REGS: Final[int] = _iow(0x82, ctypes.sizeof(kvm_regs))
KVM_GET_SREGS: Final[int] = _ior(0x83, ctypes.sizeof(kvm_sregs))
KVM_SET_SREGS: Final[int] = _iow(0x84, ctypes.sizeof(kvm_sregs))
KVM_GET_FPU: Final[int] = _ior(0x8C, ctypes.sizeof(kvm_fpu))
KVM_SET_FPU: Final[int] = _iow(0x8D, ctypes.sizeof(kvm_fpu))
KVM_SET_CPUID2: Final[int] = _iow(0x90, ctypes.sizeof(kvm_cpuid2))
KVM_GET_CPUID2: Final[int] = _iowr(0x91, ctypes.sizeof(kvm_cpuid2))
KVM_IOEVENTFD: Final[int] = _iow(0x79, ctypes.sizeof(kvm_ioeventfd))

KVM_EXIT_UNKNOWN = 0
KVM_EXIT_EXCEPTION = 1
KVM_EXIT_IO = 2
KVM_EXIT_HYPERCALL = 3
KVM_EXIT_DEBUG = 4
KVM_EXIT_HLT = 5
KVM_EXIT_MMIO = 6
KVM_EXIT_IRQ_WINDOW_OPEN = 7
KVM_EXIT_SHUTDOWN = 8
KVM_EXIT_FAIL_ENTRY = 9
KVM_EXIT_INTR = 10
KVM_EXIT_SET_TPR = 11
KVM_EXIT_TPR_ACCESS = 12
KVM_EXIT_INTERNAL_ERROR = 17
KVM_EXIT_IOAPIC_EOI = 26
KVM_EXIT_X86_RDMSR = 29
KVM_EXIT_X86_WRMSR = 30
KVM_EXIT_NOTIFY = 37
KVM_EXIT_MEMORY_FAULT = 39

KVM_EXIT_IO_IN = 0
KVM_EXIT_IO_OUT = 1

KVM_CAP_USER_MEMORY = 3
KVM_CAP_IRQFD = 32
KVM_CAP_IOEVENTFD = 36
KVM_CAP_MAX_VCPUS = 66
KVM_CAP_MAX_VCPU_ID = 128
KVM_CAP_IMMEDIATE_EXIT = 136


def ioctl(fd: int, request: int, arg: object = 0) -> int:
    """Invoke ``ioctl(2)`` and raise :class:`KvmIoctlError` on failure."""

    libc = ctypes.CDLL(None, use_errno=True)
    if isinstance(arg, (ctypes.Structure, ctypes.Union)):
        result = libc.ioctl(fd, request, ctypes.byref(arg))
    else:
        result = libc.ioctl(fd, request, arg)
    if result < 0:
        err = ctypes.get_errno()
        raise KvmIoctlError(err, os.strerror(err))
    return int(result)
