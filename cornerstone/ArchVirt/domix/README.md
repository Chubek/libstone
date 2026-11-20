# Domix

Domix is ArchVirt's bundled freestanding guest. The same `kernel.c` is compiled
for x86-64, AArch64, and RV64IM/LP64, with an architecture-specific entry sequence in
`start.S`. It implements a resident shell and filesystem in guest memory.

```sh
cmake -S . -B build
cmake --build build --target domix-image
build/archvirt --machine aarch64 --headless
```

Outputs: `build/domix/{x86_64,aarch64,riscv64}/{kernel,domix.img}`. The legacy
`build/domix/{kernel,domix.img}` paths contain the x86-64 build. Clang and LLD
cross-compile the guests without a cross-platform libc or sysroot.

## Boot and shell

DomIOS passes `ArchVirtBootInfo` in RDI on x86-64, X0 on AArch64, or a0/x10 on RV64. The kernel
validates the boot information and packed filesystem, executes `/init` as a
resident-shell startup script, and opens a serial/text console. Ctrl+F1 through
Ctrl+F4 in the ImGui window switch between four consoles; each keeps its own
screen, working directory, and input line. Files are shared across consoles.

Commands:

```text
help uname version cmdline pwd cd ls cat echo touch mkdir cp mv rm wc
clear whoami id mount df exit poweroff
```

Quoting and backslash escapes are supported. `echo text > path` writes a file;
`wc path` reports its byte count. `exit` and `poweroff` stop the guest.

The packaged root filesystem is immutable. Writes use a **volatile RAM overlay**
with 32 writable file slots of 4096 bytes each; overwrites reuse slots and
removal frees them. There are at most 128 filesystem entries, paths are limited
to 127 bytes including the leading slash, and console command lines to 255
bytes. The shell does not implement processes, executable loading, pipelines,
POSIX syscalls, or persistent storage. `tools/domixbox.c` and
`tools/domix_filebridge.c` are optional Linux-host development utilities, not
guest executables. `kernel.S` and `kernel.ld` are the earlier x86-only kernel
sources and are no longer selected by the build.

## Image format

All integers are little-endian. The 40-byte header contains `DOMIXIMG`, format
version `u32=1`, ELF machine `u32` (62, 183, or 243), kernel length `u64`, manifest
length `u64`, and entry count `u64`. Kernel ELF and manifest bytes follow, then
each entry: path length `u32`, relative UTF-8 path, mode `u32`, data length
`u64`, and data. Directory entries have mode bit `0040000` and zero data length.
Images require `/init`, fit in the 24 MiB initrd region, and cannot contain
symlinks, duplicate paths, or traversal components.
