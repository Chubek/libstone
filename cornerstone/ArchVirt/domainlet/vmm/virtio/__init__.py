"""virtio — VirtIO device domainlets.

Each module in this package models a single VirtIO device type as
defined by the VirtIO specification (third_party/virtio-spec/).

Every device domainlet exposes a ``DEVICE`` descriptor of type
:class:`VirtioDeviceDescriptor` and a ``KERNEL`` descriptor of type
:class:`VirtioKernelDescriptor`, enabling discovery and composition
by the vmm_virtio bindlet.
"""
from __future__ import annotations

from domainlet.vmm.virtio.base import (
    COMMON_FEATURES,
    COMMON_REGISTERS,
    VirtioAccess,
    VirtioDeviceDescriptor,
    VirtioDeviceStatus,
    VirtioFeature,
    VirtioKernelDescriptor,
    VirtioQueue,
    VirtioQueueDirection,
    VirtioRegister,
    VirtioRegistry,
    VirtioTransport,
)
from domainlet.vmm.virtio.catalog import DEVICE_PAIRS, REGISTRY, get_pair

from domainlet.vmm.virtio.balloon import DEVICE as BALLOON_DEVICE, KERNEL as BALLOON_KERNEL
from domainlet.vmm.virtio.blk import DEVICE as BLK_DEVICE, KERNEL as BLK_KERNEL
from domainlet.vmm.virtio.can import DEVICE as CAN_DEVICE, KERNEL as CAN_KERNEL
from domainlet.vmm.virtio.console import DEVICE as CONSOLE_DEVICE, KERNEL as CONSOLE_KERNEL
from domainlet.vmm.virtio.crypto import DEVICE as CRYPTO_DEVICE, KERNEL as CRYPTO_KERNEL
from domainlet.vmm.virtio.entropy import DEVICE as ENTROPY_DEVICE, KERNEL as ENTROPY_KERNEL
from domainlet.vmm.virtio.fs import DEVICE as FS_DEVICE, KERNEL as FS_KERNEL
from domainlet.vmm.virtio.gpio import DEVICE as GPIO_DEVICE, KERNEL as GPIO_KERNEL
from domainlet.vmm.virtio.gpu import DEVICE as GPU_DEVICE, KERNEL as GPU_KERNEL
from domainlet.vmm.virtio.i2c import DEVICE as I2C_DEVICE, KERNEL as I2C_KERNEL
from domainlet.vmm.virtio.iommu import DEVICE as IOMMU_DEVICE, KERNEL as IOMMU_KERNEL
from domainlet.vmm.virtio.input import DEVICE as INPUT_DEVICE, KERNEL as INPUT_KERNEL
from domainlet.vmm.virtio.media import DEVICE as MEDIA_DEVICE, KERNEL as MEDIA_KERNEL
from domainlet.vmm.virtio.mem import DEVICE as MEM_DEVICE, KERNEL as MEM_KERNEL
from domainlet.vmm.virtio.net import DEVICE as NET_DEVICE, KERNEL as NET_KERNEL
from domainlet.vmm.virtio.pmem import DEVICE as PMEM_DEVICE, KERNEL as PMEM_KERNEL
from domainlet.vmm.virtio.rpmb import DEVICE as RPMB_DEVICE, KERNEL as RPMB_KERNEL
from domainlet.vmm.virtio.rtc import DEVICE as RTC_DEVICE, KERNEL as RTC_KERNEL
from domainlet.vmm.virtio.scmi import DEVICE as SCMI_DEVICE, KERNEL as SCMI_KERNEL
from domainlet.vmm.virtio.scsi import DEVICE as SCSI_DEVICE, KERNEL as SCSI_KERNEL
from domainlet.vmm.virtio.sound import DEVICE as SOUND_DEVICE, KERNEL as SOUND_KERNEL
from domainlet.vmm.virtio.spi import DEVICE as SPI_DEVICE, KERNEL as SPI_KERNEL
from domainlet.vmm.virtio.vsock import DEVICE as VSOCK_DEVICE, KERNEL as VSOCK_KERNEL

__all__: list[str] = [
    "VirtioDeviceDescriptor",
    "VirtioAccess",
    "VirtioDeviceStatus",
    "VirtioFeature",
    "VirtioKernelDescriptor",
    "VirtioQueue",
    "VirtioQueueDirection",
    "VirtioRegister",
    "VirtioRegistry",
    "VirtioTransport",
    "COMMON_FEATURES",
    "COMMON_REGISTERS",
    "REGISTRY",
    "DEVICE_PAIRS",
    "get_pair",
    "BALLOON_DEVICE",
    "BALLOON_KERNEL",
    "BLK_DEVICE",
    "BLK_KERNEL",
    "CAN_DEVICE",
    "CAN_KERNEL",
    "CONSOLE_DEVICE",
    "CONSOLE_KERNEL",
    "CRYPTO_DEVICE",
    "CRYPTO_KERNEL",
    "ENTROPY_DEVICE",
    "ENTROPY_KERNEL",
    "FS_DEVICE",
    "FS_KERNEL",
    "GPIO_DEVICE",
    "GPIO_KERNEL",
    "GPU_DEVICE",
    "GPU_KERNEL",
    "I2C_DEVICE",
    "I2C_KERNEL",
    "IOMMU_DEVICE",
    "IOMMU_KERNEL",
    "INPUT_DEVICE",
    "INPUT_KERNEL",
    "MEDIA_DEVICE",
    "MEDIA_KERNEL",
    "MEM_DEVICE",
    "MEM_KERNEL",
    "NET_DEVICE",
    "NET_KERNEL",
    "PMEM_DEVICE",
    "PMEM_KERNEL",
    "RPMB_DEVICE",
    "RPMB_KERNEL",
    "RTC_DEVICE",
    "RTC_KERNEL",
    "SCMI_DEVICE",
    "SCMI_KERNEL",
    "SCSI_DEVICE",
    "SCSI_KERNEL",
    "SOUND_DEVICE",
    "SOUND_KERNEL",
    "SPI_DEVICE",
    "SPI_KERNEL",
    "VSOCK_DEVICE",
    "VSOCK_KERNEL",
]
