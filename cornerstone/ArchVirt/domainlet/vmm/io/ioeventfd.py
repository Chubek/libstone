"""ioeventfd registration helpers."""

from __future__ import annotations

import ctypes
from dataclasses import dataclass

from domainlet.vmm.core.kvm.bindings import KVM_IOEVENTFD, ioctl


class kvm_ioeventfd(ctypes.Structure):
    """ioeventfd request structure."""

    _fields_ = [
        ("datamatch", ctypes.c_uint64),
        ("addr", ctypes.c_uint64),
        ("len", ctypes.c_uint32),
        ("fd", ctypes.c_int32),
        ("flags", ctypes.c_uint32),
        ("pad", ctypes.c_uint8 * 36),
    ]


@dataclass(frozen=True, slots=True)
class IoeventFdRegistration:
    """A single ioeventfd binding."""

    addr: int
    length: int
    fd: int
    datamatch: int = 0
    pio: bool = False

    def __post_init__(self) -> None:
        if self.addr < 0 or self.length <= 0 or self.length > 8:
            raise ValueError("ioeventfd address/length is invalid")
        if self.fd < 0:
            raise ValueError("ioeventfd fd must be non-negative")
        if self.datamatch < 0 or self.datamatch > 0xFFFFFFFFFFFFFFFF:
            raise ValueError("ioeventfd datamatch must fit in uint64")


def register_ioeventfd(vm_fd: int, registration: IoeventFdRegistration) -> None:
    """Install an ioeventfd notification route."""

    req = kvm_ioeventfd()
    req.datamatch = registration.datamatch
    req.addr = registration.addr
    req.len = registration.length
    req.fd = registration.fd
    req.flags = 1 if registration.pio else 0
    ioctl(vm_fd, KVM_IOEVENTFD, req)
