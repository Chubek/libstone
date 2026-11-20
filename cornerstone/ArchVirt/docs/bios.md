# ArchVirt PC BIOS B1

B1 is an original 64 KiB x86 real-mode ROM with a legacy PC interrupt interface.
It boots a raw disk at 0000:7c00, DL=80h. Both an independent multi-sector probe
and Servo S4's real/protected/long-mode loader execute through native KVM and
ArchVirt IPC. This is a bounded polled PC platform, not a general PC-compatible
chipset capable of booting arbitrary commercial OS installation media.

## Build and select

NASM is required in addition to the existing build dependencies.

```sh
cmake -S archvirt -B archvirt/build-riscv -DCMAKE_BUILD_TYPE=Release
cmake --build archvirt/build-riscv -j6
TMPDIR=/tmp/opencode ctest --test-dir archvirt/build-riscv -R bios --output-on-failure
TMPDIR=/tmp/opencode make -C servo run-bios
archvirt/build-riscv/archvirt --machine x86_64 --backend kvm \
  --boot-mode bios --disk servo/build/servo-s4.img --headless
```

IPC boot fields: `boot_mode="bios"`, `disk="absolute/raw/image"`,
`machine="x86_64"`, `backend="kvm"`, optional `paused`, `memory_mb`, and
`firmware` (a B1-layout ROM). CLI `-b/--bios` selects that firmware path.
Direct kernel/image/cmdline, software execution, other architectures and
instruction limits are rejected in BIOS mode. `auto` may select KVM but never
falls back to software for BIOS. Default boot mode remains `direct`.
`domios`/`domix` defaults are irrelevant to BIOS selection, not guest firmware.

## Reset and memory

- RAM 40..1024 MiB. Disk 1..131072 whole 512-byte sectors (maximum 64 MiB),
  copied into private VM storage. The host image is never written.
- Reset CS selector F000h, hidden base FFFF0000h, IP FFF0h, CR0=60000010h,
  CR3/CR4/EFER zero. The far jump at physical FFFFFFF0h enters low F000:0000.
- Distinct KVM slots: low memory 0..F0000h, read-only ROM F0000h..100000h,
  high RAM 100000h..RAM end, and read-only reset alias FFFF0000h..100000000h.
  Slot IDs are unique. BIOS requires `KVM_CAP_READONLY_MEM`; CPU ROM writes
  produce a reported device failure and IPC writes to ROM reject too.
- KVM's private TSS region is FFFBD000h; identity-map page FFFBC000h. Neither
  overlaps advertised RAM or either ROM alias. These are KVM support areas,
  not the guest's eventual page tables.
- Runtime patches ROM offsets FF00h (RAM bytes), FF04h (disk sectors),
  FF48h (high usable range length), and the final checksum byte. The 64 KiB
  byte sum is zero modulo 256. A custom image must use this B1 layout; no
  third-party BIOS binary compatibility is implied.
- POST installs the IVT, initializes relevant BDA fields, masks PICs, clears
  mode-3 video, reads disk sector zero through its own INT 13h handler and
  checks bytes 510/511. No host ELF loader runs in this path. Boot DS/ES/SS=0,
  SP=7C00h, IF=1, DF=0. The platform injects no interrupts; HLT ends execution.

## Implemented BIOS services

| Interrupt | Supported function |
|---|---|
| 10h | AH=00/AL=03 mode clear; AH=02/03 cursor; AH=0E teletype; AH=0F mode query |
| 11h | Equipment word 0021h |
| 12h | Conventional memory 636 KiB |
| 13h | AH=00 reset status; 02/03 CHS read/write; 08 geometry; 41 extensions; 42/43 EDD read/write; 44 verify; 47 seek; 48 parameters |
| 15h | EAX=E820 memory map; AH=88 extended size; AX=2401 A20 enable |
| 16h | AH=00 blocking ASCII get; AH=01 peek/ZF; one buffered character |
| 19h | Restart bootstrap from sector zero |

Other vector stubs return carry with AH=86h. INT 13h rejects unsupported
functions/drive with carry and AH=01h. Unsupported INT 10h functions are ignored;
other INT 16h functions return AH=86h. This is not full video/keyboard emulation.
INT 16h returns ASCII with scan code zero, has no modifier/extended-key support,
and cannot represent NUL through the existing serial callback. VGA is an 80x25
text buffer with CR/LF/backspace, wrap and scroll, mirrored to COM1.

EDD uses 16-byte packets, count 1..127, LBA high dword zero, drive 80h. Data
buffers must lie wholly within 0500h..9F000h and not cross a 64 KiB segment.
Validation of the whole request precedes I/O. A later actual device error
would not roll back earlier sectors. The in-memory medium has no spontaneous
sector errors. Geometry translates 16 heads and 63 sectors per track; AH=08
reports whole cylinders (rejects disks smaller than one cylinder). EDD can
access remaining sectors. AH=41 reports version 20h and CX bit 0 only; verify
and seek validate medium extents, with no mechanical delay. AH=43 permits
AL=0/1; in-memory writes are immediately verifiable. Error status is returned
in the saved interrupt FLAGS so IRET does not erase carry.

E820 accepts ECX>=20, returning 20 or 24 bytes with enabled attribute 1. EBX
is an opaque index, initially zero and zero after the fourth successful record:
usable [0,9F000h), reserved [9F000h,100000h), usable [100000h,RAM end), reserved
[FFFF0000h,100000000h). Unlisted holes are not usable RAM. IVT/BDA/loader live
objects still require ownership reservations within conventional usable RAM.

## Device implementation and boundaries

`src/machine/pc_devices.cpp` implements primary ATA master ports 1F0h..1F7h
and alternate status/control 3F6h. Commands 20h/30h transfer one LBA28 sector;
firmware loops for multi-sector requests. Data is 16-bit PIO, register access
8-bit. DRQ clears after 256 words. Bad command, width or phase is rejected.
No DMA, bus-mastering, IDENTIFY, IRQ, flush/durability, secondary drive or FDC.
CHS is translated by firmware; ATA itself accepts LBA28 only.

The COM1 model supplies data, line-status, DLAB-aware divisor registers and
polled input. Port 92h reports A20 permanently enabled. PIC mask writes and
POST port 80h are accepted; there is no PIT/RTC clock, PIC routing, PCI, ACPI,
option-ROM scanning, boot menu, UEFI, USB, floppy or network boot. The historical
Nova-DOS floppy contract has therefore not become validated merely because
B1 now provides BIOS interrupts.

## Evidence

`tests/e2e/bios_boot.py` assembles an independent disk consumer, inspects reset
state and both ROM aliases, boots its CHS-loaded second stage, and checks EDD
read/write/verify/seek and rejected malformed/out-of-range/cross-segment requests.
It checks E820 at 40,64,128 MiB, repeated keyboard peek/get using injected IPC
input, VGA text, host-image immutability, invalid boot signatures, and attempted
ROM writes. Recreating the VM restores original disk contents. Native KVM is
explicit; absence may skip this CTest with code 77, never imply a firmware pass.
Servo adds actual mode transitions and the rewritten On1x kernel's reports.

Sources: supplied MuLIB BIOS/bootmgr contracts; Ralf Brown's Interrupt List
INT 13h extensions and INT 15h/E820 descriptions (ctyme HTML mirror); Intel SDM
reset/mode-transition rules; Linux KVM API sections on SREGS, memory slots,
read-only memory, TSS/identity-map support and IO/MMIO exits. Implementations
are book-owned original code; reference code was not imported.
