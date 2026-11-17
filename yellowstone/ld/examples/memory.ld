ENTRY(_start)
MEMORY {
  ROM (rx)   : ORIGIN = 0x401000, LENGTH = 4K
  RAM (rw!x) : ORIGIN = 0x405000, LENGTH = 4K
}
REGION_ALIAS("CODE", ROM)
SECTIONS {
  .text : { *(.text .text.*) } >CODE
  .rodata : { *(.rodata .rodata.*) } >ROM
  .data : ALIGN(16) { *(.data .data.*) } >RAM
  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } >RAM
}
