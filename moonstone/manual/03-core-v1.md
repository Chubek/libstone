# Core API v1

Link `Moonstone::Core` (or any component target). Core now contains compiled
services; including headers alone is no longer sufficient. Existing unversioned
C++ names resolve to the inline `moonstone::v1` namespace. C++ binary compatibility
still depends on compiler/STL ABI; the stable foreign boundary is `core/moonstone.h`.

## Sources, errors and identities

```cpp
#include <core/SourceManager.hpp>

moonstone::SourceManager sources({32, 1024 * 1024});
auto id = sources.add("example.ex", "hello\nworld");
if (!id) { /* inspect id.error().category/message/contexts */ }
else {
    auto range = sources.span(id.value(), 0, 11);
    // range.value(): example.ex, 1:1 -> 2:6, if successful.
}
```

Snapshots own their data, are immutable and outlive the manager. All source-manager
and symbol-table operations synchronize internally. Destruction must not race
operations. Source/symbol IDs start at 1, are manager/table-local and never
recycled; they are not content hashes or cross-process persistent IDs. Reinterning
an existing spelling returns the same ID, including after storage is full.

Source text and names must be strict UTF-8 (no overlong sequences, surrogates or
out-of-range scalars); names cannot contain NUL. Text may contain NUL. Ranges are
half-open byte offsets and must end on scalar boundaries. LF separates lines;
CR is an ordinary byte, including in CRLF. Columns count bytes, **not** Unicode
scalars, graphemes, display cells or LSP UTF-16 units. EOF/empty ranges are valid.
Coordinates that do not fit the existing uint32 fields fail rather than truncate.

Limits bound source/name payload and source count, and symbol/name payload and
symbol count. Index/container overhead is additional, bounded by those limits.
Semantic failures return `Result<T>` errors; C++ allocation failures may throw.
Failed insertions do not consume budgets or IDs. `Result<T>::value()` on an error
throws instead of synthesizing a default value. There is no implicit conversion
from errors or empty optionals to proof success.

## Diagnostic retention

`core/Diagnostics.hpp` supplies `DiagnosticBuffer`, a single-writer collector.
Options bound diagnostic count, retained string bytes and auxiliary entries
(related spans, notes and fixes combined). Output suppression is explicit by ID
and minimum severity. Check `suppressed()`, `dropped()`, `exhausted()` and
`has_errors()` separately: a hidden or dropped error still sets `has_errors()`.
This bounds **retained data**, not allocations made by the caller before emission
or the final serialized output size. It does not replace semantic proof status.

`apply_edits` validates half-open byte ranges and rejects overlaps and equal-start
edits (including ambiguous duplicate insertions). It does not validate UTF-8 or
AST boundaries; use a source manager/compiler mapping first. Syren still requires
an independent caller-supplied validator before labeling a repair validated.

## C embedding ABI

```c
#include <core/moonstone.h>

ms_v1_source *source = NULL;
ms_v1_span span;
if (ms_v1_source_create("example.ex", 10, "a\nb", 3, 4096, &source) == MS_V1_OK) {
    ms_v1_status status = ms_v1_source_span(source, 0, 3, &span);
    /* Only use span if status == MS_V1_OK. */
    (void)status;
    ms_v1_source_destroy(source);
}
```

ABI v1 fixes symbol names, uint32 status codes and the four-uint32 span layout
for a given target platform ABI. Windows entry points explicitly use `__cdecl`.
Future incompatible changes require new entry-point/type names. There is no
allocator exchange: inputs are copied, output structs belong to the caller, and
opaque handles are destroyed only through `ms_v1_source_destroy`. All functions
contain exceptions; allocation failure returns `MS_V1_OUT_OF_MEMORY`, other
unexpected failures `MS_V1_INTERNAL_ERROR`. Creation clears the output handle
before failure; span failure zeroes the coordinates. Zero-length strings may be
NULL. Other pointers/lengths must designate valid accessible storage; stale or
forged handles are caller errors, not a sandbox boundary. Concurrent reads of a
live source are allowed; destruction must be externally synchronized.

`MS_V1_OK` means the **source operation** succeeded, never that a program is safe.
This C ABI does not yet expose Chisel, synthesis, plugins or a collector.

## Installation

```sh
cmake -S moonstone -B build-moonstone -DBUILD_SHARED_LIBS=ON
cmake --build build-moonstone --parallel
cmake --install build-moonstone --prefix /your/prefix --component Moonstone
```

Consumers use `find_package(Moonstone 0.1 CONFIG REQUIRED COMPONENTS Core)` and
`target_link_libraries(app PRIVATE Moonstone::Core)`. Optional component names
are `Chisel`, `Syren`, `Supersonic`, `Exoml` (existing spelling). Missing requested
components fail discovery. `pkg-config --cflags --libs moonstone` is available
for conventional Unix toolchains; use a C++ linker for the static core's runtime
dependencies even when compiling a C caller. CMake handles this automatically.

Set `MOONSTONE_ENABLE_ASAN_UBSAN=ON` or `MOONSTONE_ENABLE_TSAN=ON` for local
instrumentation. They are mutually exclusive and require GCC/Clang runtimes.
Instrumented package consumers inherit sanitizer flags; do not deploy such a
package as an ordinary uninstrumented release.
