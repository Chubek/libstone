# ArchBFD application kit

ArchBFD is a compiled C ABI and a C++20 application API over the independent
qBFD readers. It inspects ELF, PE, COFF, Mach-O and `ar` archives. It neither
imports GNU BFD headers nor links libbfd. `BFD.yaml` records source-reading
findings that informed the ownership, metadata and bounds-checking contracts.

## Files and build

| File | Purpose |
| --- | --- |
| `BFDBinABI.h` | C ABI v1: opaque handles, records, constants and error results |
| `BFDBinABI.c` | C11 version/name helpers and checked endian integer access |
| `BFDBinBridge.cpp` | C++20 reader integration and exception containment |
| `BFDBinAPI.hpp` | Move-only C++ handles and collection helpers |
| `ArchBFD.h`, `ArchBFD.hpp` | Application include entry points |
| `ArchBFDApp.hpp` | Synchronous archive-aware object traversal |
| `examples/` | C section lister and C++ object/archive inspector |

From the repository root:

```sh
cmake -S yellowstone/qbfd -B /tmp/opencode/archbfd-build \
  -DQBFD_BUILD_TESTS=ON -DARCHBFD_BUILD_EXAMPLES=ON
cmake --build /tmp/opencode/archbfd-build -j2
ctest --test-dir /tmp/opencode/archbfd-build --output-on-failure
/tmp/opencode/archbfd-build/archbfd-inspect path/to/object.o path/to/library.a
cmake --install /tmp/opencode/archbfd-build --prefix /tmp/opencode/archbfd-install
```

The default is a shared library. `ARCHBFD_BUILD_SHARED=OFF` builds a static
library. `QBFD_SANITIZE=ON` instruments the library and regression tests with
ASan/UBSan on GCC/Clang. The implementation needs qBFD and the repository's
DSLtk dependency; installed application headers need only the standard library.

An installed C application can use:

```cmake
cmake_minimum_required(VERSION 3.20)
project(inspect C)
find_package(ArchBFD 1 CONFIG REQUIRED)
add_executable(inspect main.c)
target_link_libraries(inspect PRIVATE ArchBFD::archbfd)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix. For C++ applications use
`project(inspect CXX)` and `ArchBFD::cpp`, which requests C++20. For static
linking from C, enable both `C CXX` and set the executable's `LINKER_LANGUAGE`
to `CXX`; the readers require the C++ runtime. Direct shared-library consumers
can compile C with `cc -I<prefix>/include/archbfd main.c -L<prefix>/lib -larchbfd`;
configure the runtime loader path as appropriate for that installation.

The examples are also an independent consumer project:

```sh
cmake -S yellowstone/qbfd/examples -B /tmp/opencode/archbfd-apps \
  -DCMAKE_PREFIX_PATH=/tmp/opencode/archbfd-install
cmake --build /tmp/opencode/archbfd-apps
ctest --test-dir /tmp/opencode/archbfd-apps --output-on-failure
```

`ARCHBFD_EXAMPLE_C_ONLY=ON` builds the C example against the installed shared
library without enabling a C++ compiler in the consuming project.

## C API

```c
#include <ArchBFD.h>
#include <stdio.h>

int inspect(const char *path) {
  archbfd_object *file = NULL;
  archbfd_error error;
  archbfd_object_info info;
  archbfd_status status = archbfd_open_file(path, NULL, &file, &error);
  if (status == ARCHBFD_OK)
    status = archbfd_get_object_info(file, &info, &error);
  if (status == ARCHBFD_OK)
    printf("%s: %zu sections\n", archbfd_format_name(info.format), info.section_count);
  else
    fprintf(stderr, "%s\n", error.message);
  archbfd_close(file);
  return status == ARCHBFD_OK ? 0 : 1;
}
```

Pass a target name, such as `elf64-little` or `coff-x86-64`, instead of `NULL`
to request a specific reader. Format and architecture names describe parsed
metadata, not support for executing or relocating code on that machine.

## C++ API

```cpp
#include <ArchBFD.hpp>
#include <iostream>

auto file = archbfd::Binary::open("program");
for (const auto& section : file.sections()) {
  std::cout << archbfd::text(section.name) << '\n';
  auto contents = file.sectionBytes(section.index);
  auto relocations = file.relocations(section.index);
}
```

Failures throw `archbfd::Error`, with `code()` and `what()`. Empty/moved-from
handles are safe to destroy; querying one reports `ARCHBFD_INVALID_ARGUMENT`.
`ArchBFDApp.hpp` adds `visitObjects(path, visitor)`, which accepts an object or
archive and invokes `visitor(displayName, const Binary&)` for each ordinary
object member. A member that cannot be decoded reports an error, rather than
silently disappearing. Copy data needed beyond the callback's lifetime.

## ABI and ownership contract

* ABI major version is **1**, available as `ARCHBFD_ABI_VERSION` and
  `archbfd_abi_version()`. Record layouts and numeric constants are fixed for
  this major version. Records use native alignment, pointers and `size_t`:
  they are application ABI records, not portable disk or IPC encodings.
* All opens return owned opaque handles; memory opens copy their input.
  Close each successful C open exactly once. Closing `NULL` is harmless.
  C++ handles are move-only and close automatically.
* Names are pointer/length pairs; use their lengths, not `strlen`. Byte spans,
  names and record string fields are borrowed until the owning handle closes.
  Moving a C++ handle keeps those views valid. Collection vectors own their
  records, **not** the strings referenced by those records. `dependencies()`
  instead returns owning strings.
* Error storage belongs to the caller. It is optional and cleared on success;
  messages are NUL-terminated and may be truncated to 255 bytes. No C++
  exception crosses a status-returning C entry point. Allocation failure maps
  to `ARCHBFD_NO_MEMORY`. Record outputs remain unchanged on failure; opens
  always initialize their output handle to `NULL` before attempting work.
* Indices are zero-based; ELF retains its null entries. Missing references
  use `ARCHBFD_NO_INDEX`. A relocation records its owning section, static or
  dynamic symbol domain, optional target section and addend availability.
* Section bytes are raw file bytes: BSS returns an empty span, not synthesized
  zeros. VA reads are section-based, reject BSS, and cannot span sections.
  Compressed bytes stay compressed. Native relocation type/width metadata is
  not a promise that a relocation can be applied or that every instruction
  encoding's implicit addend is understood.
* Objects cache relocation records and dependencies during open so returned
  views remain stable. Immutable object queries may run concurrently if no
  thread closes the handle. Serialize archive calls, since thin-member reads
  populate a cache. If an application also uses qBFD directly, register its
  custom targets before concurrent opens; concurrent registry mutation is not
  supported.
* Archive member objects own their bytes independently and survive closing
  the archive. In-memory thin archives use `label` as their containing path;
  referenced files must remain available until opened. Index/long-name
  members have `special != 0` and are not objects.
* Universal Mach-O needs explicit selection: enumerate `machoSlices(bytes)`
  and use `Binary::machoSlice(bytes, index)`, or the corresponding C functions.
  Selection validates the table and the member CPU type and retains a copy of
  the input container.

The underlying readers' feature boundaries still apply: short ILF import
objects, BSD/Windows archive symbol-index decoding, dyld chained fixups and
ELF RELR expansion are not added by this interface. The application kit does
not expose rewriting or relocation application.
