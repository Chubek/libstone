ENTRY(_start)
SECTIONS
{
  /*
   * The packed Domix initrd is loaded at 0x00800000 and is larger than the
   * original 0x01000000 kernel placement. Keep a guard gap so the initrd
   * cannot overwrite the executable while the guest scans its filesystem.
   */
  . = 0x01c00000;
  .text : ALIGN(16) { *(.text*) }
  .rodata : ALIGN(16) { *(.rodata*) }
  .data : ALIGN(16) { *(.data*) }
  .bss : ALIGN(16) { *(COMMON) *(.bss*) }
}
