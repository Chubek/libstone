# Lapislazuli: Weavers and Collections

Lua 5.4 descriptions generate deterministic, namespaced C99 libraries:

| Weaver | Collection | Current scope |
| --- | --- | --- |
| [ArchWeave](Weavers/ArchWeave/README.md) | [ArchCollection](ArchCollection/README.md) | 36 real instruction forms across MIPS32, PowerPC32 and SPARC32 |
| [BFDWeave](Weavers/BFDWeave/README.md) | [BFDCollection](BFDCollection/README.md) | 9 structural ELF32/ELF64 readers, both byte orders |
| [VMWeave](Weavers/VMWeave/VMWeave.lua) | [VMCollection](VMCollection/README.md) | 23 executable, bounded VM profiles |

These are independent layers: **native encoding is not VM execution**, and
an ELF reader is not a linker. Native profiles use real encodings; VM profiles
have their own symbolic tapes and opcodes. None is a complete implementation
of every feature implied by an architecture/platform name.

## Generate and test

From this directory:

```sh
lua5.4 ArchCollection/regenerate.lua
lua5.4 BFDCollection/regenerate.lua
lua5.4 VMCollection/regenerate.lua
python3 test_collections.py --all
CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' python3 test_collections.py --all
```

All regenerators accept `--list`, `--check` and optional profile names. `--check`
rejects missing/stale files without writing. Absolute invocation and invocation
from within each collection work too. Edit `.archw`, `.bfdw`, `.vmw` and Weaver
sources/templates, **not generated `.c`/`.h`/`.stk00` files**. Generated code does
not depend on Lua or on binutils at runtime.

Tests require Python 3.10+, Lua 5.4, a C99 compiler and POSIX tools. They honor
`LUA`, `CC`, `CFLAGS`, isolate temporary files in `/tmp/opencode`, and check
generation, validation, all public C headers together, known encoding vectors,
operand boundaries, randomized round trips, malformed/truncated ELF files,
extended numbering, a host-compiled object, and ELF-section-to-codec handoff.
Native interfaces additionally support strict C++11 inclusion and C linkage.

## Build

```sh
cmake -S . -B /tmp/opencode/lapislazuli-build
cmake --build /tmp/opencode/lapislazuli-build --parallel
ctest --test-dir /tmp/opencode/lapislazuli-build --output-on-failure
```

`Lapislazuli::Collections` / `lapislazuli_collections` builds all 35 generated
translation units. Set `BUILD_TESTING=OFF` to build using only a C compiler and
CMake, without Python, Lua or the reference checkout. Alternatively compile
only the `.c` files for the profiles you use. This is a standalone build; it
does not change the parent libstone component selection.

## Local reference

Native profiles were developed against **`.info/binutils`**, whose
`bfd/version.h` records `BFD_VERSION_DATE 20111121`. Architecture descriptions
cite `opcodes/*-opc.c`, `include/opcode/` and `bfd/cpu-*.c`; ELF descriptions cite
target backend declarations, `include/elf/common.h` and `include/elf/external.h`.
Tests compare MIPS encodings and ELF target names/machine IDs to that checkout.
It is a reference, not a build dependency, and is never modified by generation.
The reference retains its own GNU licenses. No current-architecture coverage
(e.g. AArch64 or RISC-V) is inferred from this historical snapshot.
