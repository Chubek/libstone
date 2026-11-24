# ArchVirt

ArchVirt 0.3 boots the bundled **DomIOS + Domix** system on three explicitly supported
ArchML machine profiles: **x86_64**, **aarch64**, and **riscv64**. The frontend uses the vendored
Dear ImGui with SDL2 platform/renderer backends. All machines support headless
serial input/output.

An additional **B1 legacy PC BIOS** boot mode executes an original real-mode
ROM on native x86 KVM, with CHS/EDD disk services, E820, text/ASCII console,
and a private ATA PIO disk. It boots disk-loaded probes and Servo S4 through
ArchVirt IPC. See [the exact BIOS platform contract](docs/bios.md).

```sh
build-riscv/archvirt --machine x86_64 --backend kvm --boot-mode bios \
  --disk ../servo/build/servo-s4.img --headless
```

## Build and run

Linux dependencies: CMake 3.24+, a C++20 compiler, Make or Ninja, Perl, Python
3.13+ development headers (and tkinter for the optional bindlet generator),
Clang and LLD for the freestanding guest targets, SDL2, Lua 5.4, Zydis, and
readline, plus NASM for the PC ROM. GoogleTest enables the native unit suites.
Build from the Libstone checkout: ArchVirt uses its ArchML and DomPyBind sources
directly, and uses ImGui, TOML, and JSON from the repository's root
[`third_party/`](../../third_party/). The VirtIO specification is also kept there.
If you previously built against ArchVirt's private Libstone copy, configure
in a fresh build directory so CMake does not reuse the old dependency paths.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

build/archvirt --list-machines
build/archvirt --machine aarch64
build/archvirt --machine x86_64 --backend software
build/archvirt --machine riscv64 --backend software --headless
printf 'uname\nls /etc\ncat /etc/hostname\nexit\n' |
  build/archvirt --machine aarch64 --headless
```

`--backend auto` uses native x86 KVM when available, otherwise software.
`--backend kvm` reports initialization errors rather than selecting another
backend. AArch64 and RISC-V use software execution, including on x86 hosts.
`-V` is an alias for `--backend software`. `--max-instructions N` bounds software
execution and returns a failure if the limit is reached. Ctrl+C or closing the
window stops the VM. The default RAM size is 64 MiB; the supported range is
40–1024 MiB.

Install ArchVirt, its shared runtime dependencies, headers, machine descriptions,
fonts, and guest images with:

```sh
cmake --install build --prefix "$HOME/.local"
~/.local/bin/archvirt --list-machines
```

The installation also includes `libdomvirt` and `domvirt-shell`. Build prerequisites
listed above must be installed before configuring CMake.

Images are embedded by default. To use files from the build tree instead:

```sh
cmake -S . -B build-files -DDOMWEAVE_EMBED_DOMVIRT=OFF
cmake --build build-files -j
build-files/archvirt --machine aarch64 --headless
```

`--os PATH` overrides the packed Domix image; `--bios PATH` overrides DomIOS.
`--kernel PATH` selects an identity-mapped ELF64 executable implementing the
DomIOS boot ABI, with `--initrd PATH` supplying its data. ELF architecture,
segment bounds, executable entry point, and image bounds are checked before
execution. `--cmdline TEXT` is passed through the boot information structure.

## Machine and execution model

`machines/*.aml` contains machine extensions for ArchML's `amd64.aml`,
`arm64.aml`, and `riscv64.aml`. `tools/archml_bundle.py` combines them and adds
scalar encoding classes checked precisely by each execution adapter. The generated descriptions are
in `build/src/generated/machines/`; these may be supplied using `--archml PATH`.
An ISA-only description is insufficient: the loader checks the machine's
device layout, boot contract, ELF machine ID, width, and endianness. GPU,
bytecode, and other unimplemented machine profiles are rejected.

ArchML provides architecture metadata and instruction encoding/decoding, not
a CPU simulator. `src/machine/` selects the platform and execution adapter.
The A64 and RV64 adapters use ArchML decoding plus explicit scalar execution semantics;
the x86 adapter retains Zydis for variable-length operand/address decoding.
The software CPUs implement the integer, branch, memory, and console operations
used by the bundled system. Unsupported instructions produce diagnostics with
the guest PC.

The [RISC-V contract](docs/riscv64.md) is **RV64IM + Zifencei, LP64**, with
32 integer registers, four-byte instruction alignment, naturally aligned data,
byte MMIO serial, and DomIOS `EBREAK` terminal halt. Unsupported instructions
fail the VM with PC/encoding diagnostics. There is no SBI, privileged CSR/trap
model, MMU, interrupt controller, compressed/atomic/floating-point/vector
execution, or RISC-V KVM. Broader upstream ArchML metadata does not enable those
features. Nova-DOS N2/N3 use this backend through IPC.

These are **paravirtual DomIOS personal-computer profiles**, not complete
hardware-compatible PC/SBSA emulators. There is one CPU, flat guest addressing,
a polled serial console, and an 80×25 text surface. Arbitrary Linux/Windows
boot, UEFI, an AArch64 MMU/GIC, FP/SIMD execution, and native AArch64 KVM are not
implemented. Existing virtio/ACPI and bridge utilities remain separate
components; the bundled guest does not yet use their device protocols.

## Components and compatibility

- [`domios/`](domios/README.md): checked, architecture-neutral firmware package,
  Lua boot hook, display information table, host supervisor, and splash.
- [`domix/`](domix/README.md): shared freestanding C kernel, resident shell,
  packed root filesystem, and volatile writable overlay.
- [`include/archvirt.h`](include/archvirt.h): C API with machine/backend selection,
  serial callbacks, lifecycle functions, and error reporting.
- [`include/archvirt_boot.h`](include/archvirt_boot.h): shared guest boot ABI.

The existing `domvirt_*` C symbols, `libdomvirt.so`, CMake targets, and
`build/domvirt/domvirt` CLI path remain available. `ARCHVIRT_MACHINE` (or
`DOMVIRT_MACHINE`) selects the default machine. Existing `DOMVIRT_KERNEL`,
`DOMVIRT_INITRD`, `DOMVIRT_OS`, and `DOMVIRT_BIOS` environment paths are accepted.
Callbacks run on the execution thread; keep them bounded, and join before
releasing callback state. The x86 KVM adapter reserves `SIGRTMIN+6` to interrupt
`KVM_RUN` during host-requested shutdown.

## OS testing over IPC

`--test-server PATH` starts a persistent headless Unix-socket server. Test harnesses
can boot/reset custom kernels, send and capture serial bytes, pause/resume,
inspect registers and memory, capture the text screen, and single-step software
execution. All three machine profiles are supported; test mode defaults to software.

```python
# Run with PYTHONPATH=tools
from archvirt_ipc import TestVM

with TestVM("build/archvirt") as vm:
    vm.boot(machine="aarch64")
    vm.expect("domix# ")
    vm.send("uname\n")
    vm.expect("Domix aarch64\n")
    vm.send("exit\n")
    assert vm.wait()["state"] == "halted"
```

See [IPC testing](docs/ipc-testing.md) for custom-kernel examples, the complete
JSON protocol, limits, and Python client usage. `--control PATH` exposes the
same console/inspection commands during a normal CLI run. Existing `query-status`
and `stop` requests remain supported. File transfer still requires a guest
implementing the separate bridge protocol.

## Verification

CTest boots the actual compiled kernels on all three software adapters and, when
`/dev/kvm` is accessible, native x86 KVM. It checks shell output, filesystem
changes, console switching, shutdown, interruption, IPC-driven testing, C API boots, rejected
machine/image inputs, and ImGui rendering through SDL's dummy driver. Native
unit suites cover memory, CPU state/flags, ELF loading, firmware integrity,
Lua execution limits, bridge behavior, and device helpers.
