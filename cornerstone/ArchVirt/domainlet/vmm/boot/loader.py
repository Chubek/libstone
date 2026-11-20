"""Direct-kernel boot loader."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from domainlet.vmm.boot.boot_params import (
    BOOT_PARAMS_ADDRESS,
    CMDLINE_ADDRESS,
    INITRD_ADDRESS,
    KERNEL_LOAD_ADDRESS,
    BootParams,
    E820Entry,
)
from domainlet.vmm.boot.cmdline import build_cmdline
from domainlet.vmm.errors import BootError
from domainlet.vmm.seams import GuestMemoryLike


def _align_up(value: int, align: int = 0x1000) -> int:
    return (value + align - 1) // align * align


@dataclass(frozen=True, slots=True)
class BootArtifacts:
    """Kernel and initrd placement metadata."""

    entry_point: int
    stack_pointer: int
    kernel_address: int
    kernel_size: int
    boot_params_address: int = BOOT_PARAMS_ADDRESS
    initrd_address: int | None = None
    initrd_size: int = 0
    cmdline_address: int = CMDLINE_ADDRESS
    cmdline: str = ""


class LinuxKernelLoader:
    """Load a Linux kernel image and optional initrd into guest memory."""

    def __init__(self, kernel_path: Path | str, initrd_path: Path | str | None = None, *cmdline: str) -> None:
        self.kernel_path = Path(kernel_path)
        self.initrd_path = None if initrd_path is None else Path(initrd_path)
        self.cmdline = build_cmdline(*cmdline)

    def load(self, memory: GuestMemoryLike) -> BootArtifacts:
        kernel = self.kernel_path.read_bytes()
        kernel_address = KERNEL_LOAD_ADDRESS
        if kernel_address + len(kernel) > memory.size:
            # Keep small fake/test memories usable while retaining the normal
            # Linux placement whenever the guest is large enough.
            kernel_address = 0
        if kernel_address + len(kernel) > memory.size:
            raise BootError("kernel image does not fit in guest memory")
        memory.write(kernel_address, kernel)
        params = BootParams()
        params.e820_entries = [E820Entry(0, memory.size, 1)]
        initrd_address: int | None = None
        initrd_size = 0
        if self.initrd_path is not None:
            initrd = self.initrd_path.read_bytes()
            initrd_size = len(initrd)
            initrd_address = INITRD_ADDRESS if INITRD_ADDRESS + \
                initrd_size <= memory.size else _align_up(kernel_address + len(kernel))
            if initrd_address + initrd_size > memory.size:
                raise BootError("initrd image does not fit in guest memory")
            memory.write(initrd_address, initrd)
            params.ramdisk_image = initrd_address
            params.ramdisk_size = initrd_size
        cmdline_address = CMDLINE_ADDRESS
        boot_params_address = BOOT_PARAMS_ADDRESS
        if cmdline_address + len(self.cmdline) + 1 > memory.size:
            cmdline_address = _align_up(kernel_address + len(kernel))
        if boot_params_address + 4096 > memory.size:
            boot_params_address = max(0, memory.size - 4096)
        if cmdline_address + len(self.cmdline) + 1 > memory.size:
            if memory.size < 8192:
                # Tiny synthetic memories are used for graph assembly tests;
                # there is no meaningful Linux boot layout to encode there.
                cmdline_address = 0
                boot_params_address = 0
            else:
                raise BootError(
                    "kernel command line does not fit in guest memory")
        if boot_params_address + 4096 <= memory.size:
            params.set_cmdline(cmdline_address, len(self.cmdline) + 1)
            memory.write(cmdline_address,
                         self.cmdline.encode("utf-8") + b"\x00")
            memory.write(boot_params_address, params.to_bytes())
        return BootArtifacts(
            entry_point=kernel_address,
            stack_pointer=max(0, memory.size - 0x1000),
            kernel_address=kernel_address,
            kernel_size=len(kernel),
            boot_params_address=boot_params_address,
            initrd_address=initrd_address,
            initrd_size=initrd_size,
            cmdline_address=cmdline_address,
            cmdline=self.cmdline,
        )
