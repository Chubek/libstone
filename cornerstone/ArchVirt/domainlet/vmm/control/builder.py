"""Wiring domainlet for the VMM."""

from __future__ import annotations

from dataclasses import dataclass, field

from domainlet.vmm.boot.loader import LinuxKernelLoader
from domainlet.vmm.boot.vcpu_init import configure_vcpu
from domainlet.vmm.control.config import VmConfig
from domainlet.vmm.control.lifecycle import VmmLifecycle
from domainlet.vmm.core.allocator import AddressAllocator
from domainlet.vmm.core.guest_memory import GuestMemoryMmap
from domainlet.vmm.core.kvm.hypervisor import Hypervisor
from domainlet.vmm.core.vcpu import Vcpu
from domainlet.vmm.io.bus import Bus
from domainlet.vmm.io.event_loop import EventLoop
from domainlet.vmm.interrupts.irqchip import KvmIrqChip
from domainlet.vmm.boot.legacy.serial import SerialPort


@dataclass(slots=True)
class VmmAssembly:
    """The assembled runtime graph."""

    hypervisor: Hypervisor
    vm: object
    memory: GuestMemoryMmap
    bus: Bus
    event_loop: EventLoop
    allocator: AddressAllocator
    irqchip: KvmIrqChip
    vcpus: list[Vcpu] = field(default_factory=list)
    lifecycle: VmmLifecycle | None = None


@dataclass(slots=True)
class VmmBuilder:
    """Construct a minimal but coherent VMM graph."""

    config: VmConfig

    def build(self) -> VmmAssembly:
        hypervisor = Hypervisor()
        vm = hypervisor.create_vm(self.config.vm_type)
        memory = GuestMemoryMmap(self.config.memory_size)
        memory.register_with_vm(vm)
        bus = Bus()
        event_loop = EventLoop()
        allocator = AddressAllocator()
        irqchip = KvmIrqChip(vm)
        serial = SerialPort()
        bus.register(serial.base, serial.size, serial)
        loader = LinuxKernelLoader(
            self.config.kernel_path, self.config.initrd_path, self.config.cmdline)
        boot = loader.load(memory)
        vcpus: list[Vcpu] = []
        try:
            mmap_size = hypervisor.vcpu_mmap_size()
            for vcpu_id in range(self.config.vcpus):
                vcpu = Vcpu(vm.create_vcpu(vcpu_id), mmap_size, bus)
                configure_vcpu(vcpu, boot.entry_point,
                               boot.stack_pointer, boot.boot_params_address)
                vcpus.append(vcpu)
        except Exception:
            for vcpu in reversed(vcpus):
                vcpu.close()
            event_loop.close()
            vm.close()
            memory.close()
            hypervisor.close()
            raise
        lifecycle = VmmLifecycle(vcpus=vcpus, closeables=[
                                 *vcpus, irqchip, event_loop, vm, memory, hypervisor])
        return VmmAssembly(hypervisor, vm, memory, bus, event_loop, allocator, irqchip, vcpus, lifecycle)


def build_vmm(config: VmConfig) -> VmmAssembly:
    """Convenience wrapper."""

    return VmmBuilder(config).build()
