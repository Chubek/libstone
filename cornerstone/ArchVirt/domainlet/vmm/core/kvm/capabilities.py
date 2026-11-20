"""KVM capability probing."""

from __future__ import annotations

from dataclasses import dataclass, field

from domainlet.vmm.core.kvm.bindings import (
    KVM_CAP_IMMEDIATE_EXIT,
    KVM_CAP_IOEVENTFD,
    KVM_CAP_IRQFD,
    KVM_CAP_MAX_VCPU_ID,
    KVM_CAP_MAX_VCPUS,
    KVM_CAP_USER_MEMORY,
    KVM_CHECK_EXTENSION,
    ioctl,
)
from domainlet.vmm.errors import KvmError


@dataclass(frozen=True, slots=True)
class KvmCapabilities:
    """Probed KVM capabilities."""

    values: dict[int, int] = field(default_factory=dict)

    def require(self, capability: int) -> int:
        """Return a capability value or raise."""

        value = self.values.get(capability, 0)
        if not value:
            raise KvmError(f"missing required KVM capability {capability}")
        return value


def probe_capabilities(fd: int) -> KvmCapabilities:
    """Query the host for the KVM features this VMM relies on."""

    values = {cap: ioctl(fd, KVM_CHECK_EXTENSION, cap) for cap in (
        KVM_CAP_USER_MEMORY,
        KVM_CAP_IRQFD,
        KVM_CAP_IOEVENTFD,
        KVM_CAP_MAX_VCPUS,
        KVM_CAP_MAX_VCPU_ID,
        KVM_CAP_IMMEDIATE_EXIT,
    )}
    if not values[KVM_CAP_USER_MEMORY]:
        raise KvmError("host KVM lacks user-memory support")
    if not values[KVM_CAP_IRQFD]:
        raise KvmError("host KVM lacks irqfd support")
    if not values[KVM_CAP_IOEVENTFD]:
        raise KvmError("host KVM lacks ioeventfd support")
    if not values[KVM_CAP_MAX_VCPUS]:
        raise KvmError("host KVM lacks max-vcpu reporting")
    if not values[KVM_CAP_MAX_VCPU_ID]:
        raise KvmError("host KVM lacks max-vcpu-id reporting")
    if not values[KVM_CAP_IMMEDIATE_EXIT]:
        raise KvmError("host KVM lacks immediate-exit support")
    return KvmCapabilities(values)
