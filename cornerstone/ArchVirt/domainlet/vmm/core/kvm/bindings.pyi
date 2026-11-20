import ctypes

KVMIO: int
KVM_API_VERSION: int
KVM_GET_API_VERSION: int
KVM_CREATE_VM: int
KVM_CHECK_EXTENSION: int
KVM_GET_VCPU_MMAP_SIZE: int
KVM_CREATE_VCPU: int
KVM_SET_USER_MEMORY_REGION: int
KVM_SET_USER_MEMORY_REGION2: int
KVM_CREATE_IRQCHIP: int
KVM_IRQ_LINE: int
KVM_RUN: int
KVM_GET_REGS: int
KVM_SET_REGS: int
KVM_GET_SREGS: int
KVM_SET_SREGS: int
KVM_GET_FPU: int
KVM_SET_FPU: int
KVM_SET_CPUID2: int
KVM_GET_CPUID2: int
KVM_IOEVENTFD: int
KVM_EXIT_UNKNOWN: int
KVM_EXIT_EXCEPTION: int
KVM_EXIT_IO: int
KVM_EXIT_HLT: int
KVM_EXIT_MMIO: int
KVM_EXIT_SHUTDOWN: int
KVM_EXIT_FAIL_ENTRY: int
KVM_EXIT_INTR: int
KVM_EXIT_INTERNAL_ERROR: int
KVM_EXIT_IOAPIC_EOI: int
KVM_EXIT_X86_RDMSR: int
KVM_EXIT_X86_WRMSR: int
KVM_EXIT_NOTIFY: int
KVM_EXIT_MEMORY_FAULT: int
KVM_EXIT_IO_IN: int
KVM_EXIT_IO_OUT: int
KVM_CAP_USER_MEMORY: int
KVM_CAP_IRQFD: int
KVM_CAP_IOEVENTFD: int
KVM_CAP_MAX_VCPUS: int
KVM_CAP_MAX_VCPU_ID: int
KVM_CAP_IMMEDIATE_EXIT: int

class KvmIoctlError(OSError): ...
class kvm_userspace_memory_region(ctypes.Structure): ...
class kvm_userspace_memory_region2(ctypes.Structure): ...
class kvm_regs(ctypes.Structure): ...
class kvm_segment(ctypes.Structure): ...
class kvm_dtable(ctypes.Structure): ...
class kvm_sregs(ctypes.Structure): ...
class kvm_fpu(ctypes.Structure): ...
class kvm_cpuid_entry2(ctypes.Structure): ...
class kvm_cpuid2(ctypes.Structure): ...
class kvm_irq_level(ctypes.Structure): ...
class kvm_ioeventfd(ctypes.Structure): ...
class kvm_run(ctypes.Structure): ...

def ioctl(fd: int, request: int, arg: object = ...) -> int: ...
