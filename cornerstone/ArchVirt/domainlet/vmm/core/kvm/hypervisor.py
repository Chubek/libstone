"""/dev/kvm handle wrappers."""

from __future__ import annotations

import os
from dataclasses import dataclass, field

from domainlet.vmm.core.kvm.bindings import (
    KVM_API_VERSION,
    KVM_CREATE_IRQCHIP,
    KVM_CREATE_VCPU,
    KVM_CREATE_VM,
    KVM_GET_API_VERSION,
    KVM_GET_VCPU_MMAP_SIZE,
    KVM_SET_USER_MEMORY_REGION,
    ioctl,
    kvm_userspace_memory_region,
)
from domainlet.vmm.core.kvm.capabilities import KvmCapabilities, probe_capabilities
from domainlet.vmm.errors import KvmError


@dataclass(slots=True)
class Hypervisor:
    """Process-local access to `/dev/kvm`."""

    path: str = "/dev/kvm"
    fd: int = field(init=False)
    capabilities: KvmCapabilities = field(init=False)

    def __post_init__(self) -> None:
        try:
            self.fd = os.open(self.path, os.O_RDWR | os.O_CLOEXEC)
        except OSError as exc:
            raise KvmError(f"unable to open {self.path}: {exc}") from exc
        try:
            version = ioctl(self.fd, KVM_GET_API_VERSION)
            if version != KVM_API_VERSION:
                raise KvmError(f"unexpected KVM API version {version}")
            self.capabilities = probe_capabilities(self.fd)
        except Exception:
            os.close(self.fd)
            self.fd = -1
            raise

    def close(self) -> None:
        """Close the host KVM fd."""

        if getattr(self, "fd", -1) >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self) -> Hypervisor:
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def create_vm(self, vm_type: int = 0) -> VirtualMachine:
        """Create a VM fd."""

        return VirtualMachine(ioctl(self.fd, KVM_CREATE_VM, vm_type), self)

    def vcpu_mmap_size(self) -> int:
        """Return the vCPU shared-run-page size."""

        return ioctl(self.fd, KVM_GET_VCPU_MMAP_SIZE)


@dataclass(slots=True)
class VirtualMachine:
    """A KVM VM fd with convenience wrappers."""

    fd: int
    hypervisor: Hypervisor

    def close(self) -> None:
        """Close the VM fd."""

        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def create_vcpu(self, vcpu_id: int) -> int:
        """Create and return a vCPU fd."""

        return ioctl(self.fd, KVM_CREATE_VCPU, vcpu_id)

    def create_irqchip(self) -> None:
        """Request KVM in-kernel interrupt controller emulation."""

        ioctl(self.fd, KVM_CREATE_IRQCHIP)

    def set_user_memory_region(self, region: kvm_userspace_memory_region) -> None:
        """Install a guest memory slot."""

        ioctl(self.fd, KVM_SET_USER_MEMORY_REGION, region)

    def __enter__(self) -> VirtualMachine:
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()
