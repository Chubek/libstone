# qobjld link backends

`qobjld` defaults to its **own in-process linker**, not a toolchain subprocess.
Static ELF executables/partial links, native ELF64 x86-64 dynamic images,
PE x86/x64 executables, and static Mach-O x86-64 executables can be built
without `ld`, LLD, LLVM libraries, a compiler, or any tools on `PATH` at link
time.

This is a bounded native implementation, **not full GNU ld/LLD parity**:

| Native format | Implemented | Explicitly unsupported |
| --- | --- | --- |
| ELF | Static/partial emitter and scripts; ELF64 x86-64 shared objects, PIE and fixed-address dynamic executables; PIC GOT/PLT, runtime RELA, DSO imports, `DT_NEEDED`/SONAME/RUNPATH/RPATH, SysV/GNU hashes, init/fini arrays, `-z now` and RELRO; static archives and `-L/-l` | Native dynamic TLS, IFUNC, symbol versions, copy relocations, dynamic linker scripts, COMDAT/LTO and non-x86-64 dynamic output; unsupported relocations |
| PE/COFF | x86/x64 fixed-base console images, static archives, absolute/relative/RVA/section relocations, x64 `.pdata` ordering | DLLs/imports/exports, COMDAT, weak externals, TLS/CRT directives, ASLR, partial links, section GC |
| Mach-O | x86-64 fixed-base `MH_EXECUTE`, `LC_UNIXTHREAD`, segments, symbols, static archives, unsigned/signed/branch relocations | Dylibs/imports, PIE, ARM64, GOT/TLV/subtractor relocations, unwind/initializers, code signing, partial links, section GC |

Native PE/Mach-O images are limited to 256 MiB and section alignments up to
64 KiB (PE up to 4 KiB). An executable entry must name code; the default is
`_start`. Use `-e entry` for PE or `-e _entry` for Mach-O when appropriate.
Unsupported requests fail before publishing the image; there is **no automatic
external fallback**, even when compatibility bridges are built.

The optional external bridges below preserve broader dynamic/toolchain
functionality for callers that explicitly opt into that dependency. Native
Windows/Darwin loader execution has not been validated; Linux tests independently
map and execute freestanding x86-64 fixture code from both emitted formats.

## Selection and build

Object **contents**, including archive members, select PE or Mach-O automatically.
Use `--format=elf|pe|macho` for library-only links or explicit selection.
LLVM bitcode is not a native input. Mixed object formats are errors. `--format=auto` uses input
discovery. Library-only C/C++ calls should select the appropriate backend API.

```sh
cmake -S yellowstone -B build-yellowstone \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON \
  -DQOBJFILE_DOMTERM=OFF
cmake --build build-yellowstone --parallel
ctest --test-dir build-yellowstone --output-on-failure
```

| CMake option | Default |
| --- | --- |
| `YELLOWSTONE_ENABLE_ELF_BACKEND` (external bridge) | OFF |
| `YELLOWSTONE_ENABLE_PE_BACKEND` (external bridge) | OFF |
| `YELLOWSTONE_ENABLE_MACHO_BACKEND` (external bridge) | OFF |
| `YELLOWSTONE_ELF_LINKER` | `ld.bfd` |
| `YELLOWSTONE_PE_LINKER` | `lld-link` |
| `YELLOWSTONE_MACHO_LINKER` | `ld64.lld` |

`--elf-linker`, `--pe-linker`, and `--macho-linker` explicitly request external
execution. For compatibility, `--dynamic` still means “use the configured
external linker”; it is not required for native dynamic ELF output. Use
`--shared`, `--pie`, or an explicit ELF DSO input for the native path. Build the
relevant `YELLOWSTONE_ENABLE_*_BACKEND=ON` before selecting an external bridge.
Setting those CMake options alone does **not** change native defaults.
An unavailable executable or disabled bridge produces an error. PE and Mach-O
process launch uses POSIX spawning or Windows `CreateProcessW`, without a shell;
the Windows host implementation has not yet been validated on a Windows runner.

Building qld requires Perl (core modules only): the repository's
`scripts/aurocks.pl` and `scripts/asdl.pl` generate its C11 expression parser and
arena-owned AST from `ldscript.g` and `ldscript.asdl`. These are build-time
dependencies only. Lua 5.5 and Kaguya come from `third_party` through
`cmake/LibstoneLua.cmake`, not a separately installed Lua ABI.

## Native dynamic ELF

The in-process dynamic backend currently targets little-endian ELF64 x86-64.
It emits genuine loader metadata rather than changing `e_type` on a static
image:

- `PT_PHDR`, `PT_INTERP`, `PT_LOAD`, `PT_DYNAMIC`, `PT_GNU_STACK`, and optional
  `PT_GNU_RELRO`;
- `.dynsym`/`.dynstr`, SysV and/or GNU hash tables, `.dynamic`,
  `.rela.dyn`/`.rela.plt`, GOT and traditional x86-64 PLT entries;
- `R_X86_64_GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX`, `PLT32`, `PC32`, `PC64`,
  and safe `64`/`RELATIVE` runtime relocation paths;
- DSO SONAME-based dependencies, `-L/-l` shared-library discovery,
  `--as-needed`, RUNPATH/RPATH, constructors/destructors, lazy or eager PLT
  binding, and shared/PIE symbol preemption.

For a shared object:

```sh
cc -fPIC -c provider.c -o provider.o
qobjld --shared --soname=libprovider.so --hash-style=both \
  provider.o -o libprovider.so
```

For a PIE or fixed-address dynamic executable, pass DSOs directly or through
`-L/-l`. PIE defaults to `/lib64/ld-linux-x86-64.so.2`; override it with
`--dynamic-linker` when targeting a different x86-64 Linux runtime:

```sh
cc -fPIE -c start.s -o start.o
qobjld --pie -z now -z relro --rpath='$ORIGIN' \
  start.o -L. -lprovider -o program
```

Non-PIC absolute/32-bit relocations that cannot remain correct after load-base
selection are rejected with a “recompile as PIC/PIE” diagnostic. Dynamic TLS,
IFUNC, symbol versioning, copy relocations, text relocations and native dynamic
linker scripts are deliberately unsupported rather than approximated. Use the
explicit external bridge when those features are required. ELF library search
prefers `.so` by default; `--static` or `-Bstatic` selects archives.

## Native static ELF and script mapping

The optional `ElfBackend.cpp` implements the external contract in `Backend.hpp`; ELF, PE, and
Mach-O share bounded response expansion, process execution, diagnostic handling,
and auxiliary-output collision/publication helpers. `ElfBackend.hpp` remains a
compatibility include. ELF validates the requested output kind after a successful
toolchain exit; explicitly static output must have no interpreter/dependencies.
This remains a toolchain compatibility bridge, separate from `DynamicElf.cpp`'s
bounded in-process dynamic linker.

The in-process static frontend supports `MEMORY`, `REGION_ALIAS`, `SECTIONS`,
`NOLOAD`, section `ALIGN`, ordered data directives, and `*(COMMON)` allocation.
Aliases share their region's allocation cursor and work with `ORIGIN`/`LENGTH`.
For sections without an explicit address/region, memory attributes select the
first matching declared region. Region bounds include alignment and COMMON data.
`--Map` includes memory configuration, aliases, and input/output section mapping.
Empty file-backed sections no longer corrupt BSS-only ELF LOAD geometry.
Native partial (`-r`) links also preserve script-emitted data/padding.

The section/directive parser remains recursive descent; **expression parsing**
is generated by Aurocks/ASDL. It supports C operator precedence, unsigned 64-bit
constants, K/M/G suffixes, and source-position diagnostics. Arena ownership
cleans up speculative/failed parses. A private mutex serializes the generated
parser's global depth/error state; separate link contexts retain independent ASTs.
Unsupported native script semantics still produce diagnostics.

Lua has equivalent memory/section builders, including table-form section options:

```lua
qld.memory('ROM', '0x401000', '4K', 'rx')
qld.region_alias('CODE', 'ROM')
qld.section('.text', {patterns={'.text', '.text.*'}, region='CODE', keep=true})
```

Executable Linux x86-64 examples (run from the repository root after building):

```sh
cc -c yellowstone/ld/examples/memory.s -o /tmp/opencode/qld-memory.o
build-yellowstone/ld/qobjld -T yellowstone/ld/examples/memory.ld \
  /tmp/opencode/qld-memory.o --Map /tmp/opencode/qld-memory.map -o /tmp/opencode/qld-memory
/tmp/opencode/qld-memory # exits 42
build-yellowstone/ld/qobjld -T yellowstone/ld/examples/memory.lua \
  /tmp/opencode/qld-memory.o -o /tmp/opencode/qld-memory-lua
/tmp/opencode/qld-memory-lua # exits 42
```

Both example layouts are compiled, linked and executed by the script tests.
See [the script manual](../manual/19-scripts.md) for placement rules and limits.

## Native C11 SDK example

`examples/native-sdk.c` links objects/archives using automatic format detection.
It is compiled and executed by `qld-native` for all three formats, with `PATH`
empty and subprocess invocation forbidden. For an installed SDK:

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I/tmp/opencode/qld-native-install/include/qobjld \
  -c yellowstone/ld/examples/native-sdk.c -o /tmp/opencode/native-sdk.o
c++ /tmp/opencode/native-sdk.o -L/tmp/opencode/qld-native-install/lib \
  -lqld -llibstone_script_lua -ldl -lm -pthread -o /tmp/opencode/native-sdk
# Use the memory.o assembled in the earlier example:
env PATH=/nonexistent /tmp/opencode/native-sdk _start \
  /tmp/opencode/native-program /tmp/opencode/qld-memory.o
/tmp/opencode/native-program # exits 42
```

For native cross-format C input, compile freestanding code with no runtime
dependencies. Mach-O inputs must omit unwind tables (for example Clang's
`-fno-asynchronous-unwind-tables`); otherwise qld rejects their unsupported
unwind metadata. PE/Mach-O debug input sections are currently omitted.

## Optional external PE/COFF bridge

With the compatibility bridge enabled and explicitly selected:

- Executables and DLLs; x86, x64 and ARM64 output are regression-tested.
- Static `.lib`/`.a` archive extraction, COMDAT selection, relocation and base
  relocations; section GC via `/opt:ref` or `--gc-sections`.
- Function/data imports, import libraries, `__declspec(dllexport)`, `/export`,
  `.def` ordinal exports and forwarded exports.
- Native subsystem, entry, image base, CRT/default-library, TLS, unwind,
  delay-load and other linker options. TLS and delay-load runtime behavior
  requires the appropriate target CRT/SDK and is not runtime-tested here.
- Import-library, map and PDB output. DLL export names and import-library DLL
  names retain the requested output basename, never the temporary staging path.

Common qld flags are translated: `--shared`, `-e`, `-L`, `-l`, `--image-base`,
`--subsystem`, `--export`, `--out-implib`, `--pie`, `--strip-debug`, `-Map`,
`--allow-undefined`. Native `/option:value` flags pass through in order.
`-lfoo` searches `foo.lib`, `libfoo.lib`, `libfoo.a`, `libfoo.dll.a` in the `-L`
directories, then falls back to native `foo.lib` lookup. `-l:filename` selects
an exact filename. `/defaultlib` and `/nodefaultlib` retain native semantics.
GNU `-Bstatic/-Bdynamic` are not PE options; choose static/import libraries
explicitly. `--static` additionally verifies the image has neither an import
directory nor a delay-import directory; ASLR is independent of this check.

Self-contained cross-link example (from the repository root):

```sh
cat > provider.c <<'EOF'
__declspec(dllexport) int answer(void) { return 42; }
EOF
clang -target x86_64-pc-windows-msvc -c provider.c -o provider.obj
build-yellowstone/ld/qobjld --pe-linker=lld-link provider.obj --shared /noentry -o provider.dll
# Also creates provider.lib.
cat > consumer.c <<'EOF'
__declspec(dllimport) int answer(void);
int entry(void) { return answer(); }
EOF
clang -target x86_64-pc-windows-msvc -c consumer.c -o consumer.obj
build-yellowstone/ld/qobjld --pe-linker=lld-link consumer.obj provider.lib \
  /entry:entry /subsystem:console /nodefaultlib -o consumer.exe
llvm-readobj --coff-exports provider.dll
llvm-readobj --coff-imports consumer.exe
```

`/noentry` is explicit: a normal DLL may need `DllMainCRTStartup` and CRT
initialization. Partial COFF links (`-r`) are unsupported by `lld-link`; use
`qobjar`/`llvm-ar` to create static libraries.

## Optional external Mach-O bridge

- Executables, dylibs and bundles; x86-64 and ARM64 cross-links are tested.
- Static archive extraction and `-force_load`/`-all_load`, native library and
  framework search, dylib install names, versions, rpaths, weak imports,
  reexports, two-level namespace, export filters and runtime symbol lookup.
- Native relocations, GOT/non-lazy bindings, lazy stubs and thread-local
  variable descriptors; the tests inspect real emitted binding tables.
- Native `-arch` and `-platform_version` are explicit. Target SDK, CRT,
  libSystem and framework paths belong to the caller/compiler driver.

```sh
cat > answer.c <<'EOF'
int answer(void) { return 42; }
EOF
clang -target x86_64-apple-macos11 -c answer.c -o answer.o
build-yellowstone/ld/qobjld --macho-linker=ld64.lld answer.o --shared -arch x86_64 \
  -platform_version macos 11.0 11.0 \
  -install_name @rpath/libanswer.dylib -o libanswer.dylib
llvm-objdump --macho --private-headers libanswer.dylib

# Statically link the function from an archive into an executable.
llvm-ar --format=darwin rcs libanswer.a answer.o
cat > main.c <<'EOF'
extern int answer(void);
int main(void) { return answer(); }
EOF
clang -target x86_64-apple-macos11 -c main.c -o main.o
build-yellowstone/ld/qobjld --macho-linker=ld64.lld main.o libanswer.a -arch x86_64 \
  -platform_version macos 11.0 11.0 -o answer-macho
llvm-nm answer-macho
```

Linking static archives into a Mach-O image is distinct from producing a
fully static Darwin executable. LLD 22 does **not** implement `-static` or
`-r`. qld checks the actual Mach-O file type and `MH_DYLDLINK` flag and rejects
ignored requests. A configured ld64-compatible toolchain may implement these
modes; its output must pass the same checks. No universal/fat image assembly
is provided; link each architecture separately and use a suitable fat-binary
tool. GNU/Lua linker scripts are unsupported in PE/Mach-O; use native section
options (or ELF for the script SDK).

## APIs and output lifetime

The original `qld_options_t` ABI is unchanged. New C entry points mirror ELF:

- `qld_pe_options_init`, `qld_link_pe`, `qld_link_pe_bytes`, `qld_set_pe_options`,
  `qld_pe_linker`.
- `qld_macho_options_init`, `qld_link_macho`, `qld_link_macho_bytes`,
  `qld_set_macho_options`, `qld_macho_linker`.

The PE/Mach-O v1 option records have the same layout as `qld_elf_options_t`.
Initialize them before use. A NULL executable selects native linking, including
when calling `qld_link_elf`, `qld_link_pe`, or `qld_link_macho`. A non-NULL
executable explicitly selects the corresponding external compatibility bridge.
`qld_*_linker()` reports its configured external executable (NULL if disabled),
**not** native format availability. Each record contains an executable, an ordered
argv vector and a diagnostic callback. Structured options come first, then
the argv vector. Setters copy strings; a successful setter replaces any
previous backend selection. Passing NULL restores automatic selection.
Invalid configurations leave the builder unchanged. Release returned bytes
and diagnostics with `qld_free_bytes` and `qld_free_string`.

C++ exposes `Options::pe`, `Options::macho`, `PeOptions`, `MachOOptions`,
`Linker::setPeOptions` and `Linker::setMachOOptions`. Selecting multiple
backends in one `Options` is an error.

`examples/formats-sdk.c` is a compiled, executed C11 example. C++ and
incremental calls are exercised by `tests/formats_api.cpp`.

For external bridges, output bytes are read only after successful process exit and format/kind
validation. Primary output, PE import libraries/PDBs/maps/manifests and
Mach-O maps are staged in a private workspace. Requested auxiliary
destinations are published after validation. A file-producing PE link
defaults to `<output-stem>.lib` when exports exist. For memory-only links,
auxiliary files are discarded unless explicit destinations were supplied.
Output-path collisions are errors. Publication is not a multi-file atomic
filesystem transaction. Other native side outputs (LTO caches, reproducers,
dependency logs, etc.) retain their native toolchain behavior.

PE response files use Windows quoting; select `--format=pe` when the entire
invocation is inside a response file. Mach-O response files use GNU quoting.
Nested expansion is bounded and cycles/NULs/malformed quotes are errors.
`@rpath`, `@loader_path` and `@executable_path` remain literal Mach-O tokens.
Successful-link warnings are delivered through the diagnostic callback.
Known “ignored/unsupported option” warnings are errors, rather than silent
success.

See [VALIDATION.md](VALIDATION.md) for tested configurations and remaining
platform coverage.
