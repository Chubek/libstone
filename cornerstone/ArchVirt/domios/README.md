# DomIOS

DomIOS is the host-side firmware supervisor and boot package for ArchVirt's
x86-64, AArch64, and RISC-V machines. Its package contains architecture-neutral data;
host executables are no longer embedded as guest firmware.

## Boot flow

1. Validate the `DOMIOS2` header, section bounds, section types, and checksums.
2. Execute the packaged Lua boot hook with `arg[1]` set to the selected machine.
3. Copy the checked firmware package to guest RAM and publish a display
   information table (DIT).
4. Load the matching Domix ELF and filesystem, publish `ArchVirtBootInfo`, and
   enter the kernel with its boot-information pointer.

The host splash is presented through the ImGui frontend. Lua programs execute
in fresh Lua 5.4 states with argument passing, limited libraries, an 8 MiB
allocation budget, and a one-million-instruction budget. Runtime errors are
reported through the event queue and firmware boot errors prevent handoff.
The native plugin loader validates the plugin ABI and shuts down plugins in
reverse initialization order.

## Targets and files

- `domios`: static supervisor library.
- `domios_bootstrap_stub`: host utility for generating the DIT/splash.
- `mkimg`: firmware package builder.
- `domios_image`: emits `build/domios/domios.img`.
- `domios_tests`: supervisor smoke tests.
- `src/firmware.*`: shared packer/runtime validation.
- `../include/archvirt_boot.h`: versioned guest handoff ABI.

`DOMIOS2` is a little-endian package with an eight-byte magic, `u32` version 2,
and `u32` section count 5. Five 24-byte entries contain type `u32`, CRC32 `u32`,
offset `u64`, and length `u64`. Payloads are contiguous and contain the DIT,
logo, background, Lua hook, and manifest. The old offset-only firmware image
format is rejected; rebuild or replace overrides with the generated image.

The guest's flat memory layout places the DIT at `0x1000`, boot information at
`0x10000`, command line at `0x11000`, kernel at `0x200000`, initial stack top at
`0x700000`, filesystem image at `0x800000`, and firmware at `0x2000000`.
The console surface is at `0xb8000`; serial uses x86 port `0x3f8` or AArch64/RV64
MMIO byte address `0x10000000`.
