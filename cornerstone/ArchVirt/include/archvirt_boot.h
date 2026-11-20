#ifndef ARCHVIRT_BOOT_H
#define ARCHVIRT_BOOT_H
#include <stdint.h>

/* DomIOS v1: little-endian, flat physical addressing, one bootstrap CPU.
 * Passed in RDI (amd64), X0 (AArch64), or a0/x10 (RV64). RV64 starts with
 * sp=0x700000, other GPRs zero, IALIGN=32; it is not the Linux/SBI entry ABI.
 * No host pointers appear in this ABI. */
#define ARCHVIRT_BOOT_MAGIC UINT64_C(0x31544f4f42565241)
struct ArchVirtBootInfo {
    uint64_t magic;
    uint32_t version;
    uint16_t architecture; /* ELF e_machine */
    uint16_t flags;        /* bit 0: validated DomIOS firmware supplied */
    uint64_t memory_size;
    uint64_t image_base;
    uint64_t image_size;
    uint64_t firmware_base;
    uint64_t firmware_size;
    uint64_t console_base;
    uint32_t console_columns;
    uint32_t console_rows;
    uint64_t serial_base;
    uint64_t cmdline_base;
    uint64_t cmdline_size;
};
#endif
