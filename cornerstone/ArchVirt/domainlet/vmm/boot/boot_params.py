"""Linux x86 boot parameter image helpers."""

from __future__ import annotations

import ctypes
from dataclasses import dataclass, field

BOOT_PARAMS_ADDRESS = 0x90000
CMDLINE_ADDRESS = 0x20000
INITRD_ADDRESS = 0x4000000
KERNEL_LOAD_ADDRESS = 0x100000
BOOT_FLAG = 0xAA55
SETUP_HEADER_MAGIC = 0x53726448
LOADFLAG_LOADED_HIGH = 1 << 0
LOADFLAG_CAN_USE_HEAP = 1 << 7
XLF_KERNEL_64 = 1 << 0
XLF_CAN_BE_LOADED_ABOVE_4G = 1 << 1


class SetupHeader(ctypes.LittleEndianStructure):
    """Linux setup header."""

    _layout_ = "ms"
    _pack_ = 1
    _fields_ = [
        ("setup_sects", ctypes.c_uint8),
        ("root_flags", ctypes.c_uint16),
        ("syssize", ctypes.c_uint32),
        ("ram_size", ctypes.c_uint16),
        ("vid_mode", ctypes.c_uint16),
        ("root_dev", ctypes.c_uint16),
        ("boot_flag", ctypes.c_uint16),
        ("jump", ctypes.c_uint16),
        ("header", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("realmode_swtch", ctypes.c_uint32),
        ("start_sys_seg", ctypes.c_uint16),
        ("kernel_version", ctypes.c_uint16),
        ("type_of_loader", ctypes.c_uint8),
        ("loadflags", ctypes.c_uint8),
        ("setup_move_size", ctypes.c_uint16),
        ("code32_start", ctypes.c_uint32),
        ("ramdisk_image", ctypes.c_uint32),
        ("ramdisk_size", ctypes.c_uint32),
        ("bootsect_kludge", ctypes.c_uint32),
        ("heap_end_ptr", ctypes.c_uint16),
        ("ext_loader_ver", ctypes.c_uint8),
        ("ext_loader_type", ctypes.c_uint8),
        ("cmd_line_ptr", ctypes.c_uint32),
        ("initrd_addr_max", ctypes.c_uint32),
        ("kernel_alignment", ctypes.c_uint32),
        ("relocatable_kernel", ctypes.c_uint8),
        ("min_alignment", ctypes.c_uint8),
        ("xloadflags", ctypes.c_uint16),
        ("cmdline_size", ctypes.c_uint32),
        ("hardware_subarch", ctypes.c_uint32),
        ("hardware_subarch_data", ctypes.c_uint64),
        ("payload_offset", ctypes.c_uint32),
        ("payload_length", ctypes.c_uint32),
        ("setup_data", ctypes.c_uint64),
        ("pref_address", ctypes.c_uint64),
        ("init_size", ctypes.c_uint32),
        ("handover_offset", ctypes.c_uint32),
        ("kernel_info_offset", ctypes.c_uint32),
    ]


class BootE820Entry(ctypes.LittleEndianStructure):
    """An e820 memory map entry."""

    _layout_ = "ms"
    _pack_ = 1
    _fields_ = [
        ("addr", ctypes.c_uint64),
        ("size", ctypes.c_uint64),
        ("type", ctypes.c_uint32),
    ]


@dataclass(frozen=True, slots=True)
class E820Entry:
    """High-level e820 entry."""

    addr: int
    size: int
    type: int = 1

    def __post_init__(self) -> None:
        if self.addr < 0 or self.size <= 0:
            raise ValueError(
                "e820 address must be non-negative and size positive")
        if self.type <= 0:
            raise ValueError("e820 type must be positive")

    def to_struct(self) -> BootE820Entry:
        entry = BootE820Entry()
        entry.addr = self.addr
        entry.size = self.size
        entry.type = self.type
        return entry


@dataclass(slots=True)
class BootParams:
    """Serializable boot-parameter image."""

    cmdline_ptr: int = CMDLINE_ADDRESS
    ramdisk_image: int = INITRD_ADDRESS
    ramdisk_size: int = 0
    e820_entries: list[E820Entry] = field(default_factory=list)
    setup_header: SetupHeader = field(default_factory=SetupHeader)

    def __post_init__(self) -> None:
        if self.cmdline_ptr < 0 or self.ramdisk_image < 0 or self.ramdisk_size < 0:
            raise ValueError(
                "boot parameter addresses and sizes must be non-negative")
        if len(self.e820_entries) > 128:
            raise ValueError("at most 128 e820 entries are supported")
        self.setup_header.boot_flag = BOOT_FLAG
        self.setup_header.header = SETUP_HEADER_MAGIC
        self.setup_header.type_of_loader = 0xFF
        self.setup_header.loadflags = LOADFLAG_LOADED_HIGH | LOADFLAG_CAN_USE_HEAP
        self.setup_header.xloadflags = XLF_KERNEL_64 | XLF_CAN_BE_LOADED_ABOVE_4G
        self.setup_header.cmd_line_ptr = self.cmdline_ptr
        self.setup_header.ramdisk_image = self.ramdisk_image
        self.setup_header.ramdisk_size = self.ramdisk_size

    def to_bytes(self) -> bytes:
        """Serialize the zeropage image."""

        size = 4096
        blob = bytearray(size)
        blob[0x1F1: 0x1F1 +
             ctypes.sizeof(self.setup_header)] = bytes(self.setup_header)
        blob[0x1E8] = len(self.e820_entries) & 0xFF
        blob[0x1EF] = 0xFF
        offset = 0x2D0
        for entry in self.e820_entries[:128]:
            item = entry.to_struct()
            blob[offset: offset + ctypes.sizeof(item)] = bytes(item)
            offset += ctypes.sizeof(item)
        return bytes(blob)

    def set_cmdline(self, ptr: int, size: int) -> None:
        if ptr < 0 or size < 0:
            raise ValueError(
                "command line pointer and size must be non-negative")
        self.setup_header.cmd_line_ptr = ptr
        self.setup_header.cmdline_size = size
