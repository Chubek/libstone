from dataclasses import dataclass

from domainlet.vmm.boot.legacy.serial import SerialPort
from domainlet.vmm.control.config import VmConfig
from domainlet.vmm.control.lifecycle import VmmLifecycle
from domainlet.vmm.core.allocator import AddressAllocator
from domainlet.vmm.core.guest_memory import GuestMemoryMmap
from domainlet.vmm.core.kvm.hypervisor import Hypervisor
from domainlet.vmm.core.vcpu import Vcpu
from domainlet.vmm.io.bus import Bus
from domainlet.vmm.io.event_loop import EventLoop
from domainlet.vmm.interrupts.irqchip import KvmIrqChip

@dataclass
class VmmAssembly:
    hypervisor: Hypervisor
    vm: object
    memory: GuestMemoryMmap
    bus: Bus
    event_loop: EventLoop
    allocator: AddressAllocator
    irqchip: KvmIrqChip
    vcpus: list[Vcpu]
    lifecycle: VmmLifecycle | None = ...

@dataclass
class VmmBuilder:
    config: VmConfig
    def build(self) -> VmmAssembly: ...

def build_vmm(config: VmConfig) -> VmmAssembly: ...
