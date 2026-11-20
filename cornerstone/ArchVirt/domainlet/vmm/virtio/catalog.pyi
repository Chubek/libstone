from .base import VirtioDeviceDescriptor, VirtioKernelDescriptor, VirtioRegistry

DEVICE_PAIRS: dict[str, tuple[VirtioDeviceDescriptor, VirtioKernelDescriptor]]
REGISTRY: VirtioRegistry

def get_pair(name: str) -> tuple[VirtioDeviceDescriptor, VirtioKernelDescriptor]: ...

__all__: list[str]
