ENTRY(_start)
SECTIONS {
    . = 0x200000;
    .text : { *(.text.boot) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
    ASSERT(. <= 0x500000, "guest overlaps ArchVirt result mailbox")
}
