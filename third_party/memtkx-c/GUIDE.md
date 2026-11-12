# MemTKX Guide

## A 15-chapter usage and plugin manual

This manual teaches MemTKX from raw allocation to object tracing,
collection orchestration, and custom extensions. It describes the C99 API
in this repository, including how the current collection plans actually
behave, and closes with the header-only C++ wrapper.

For the shortest path to a working program, read [README.md](README.md). For
a first runtime integration, work through chapters 1–7 and then 9–11.
Chapter 13 covers plugins and chapter 14 covers the C++ wrapper. Chapter 8
explains which plan fits which experiment.

### Contents

1. [Identity and architecture](#chapter-1--identity-and-architecture)
2. [Building and integrating](#chapter-2--building-and-integrating)
3. [Addresses, units, and results](#chapter-3--addresses-units-and-results)
4. [Object lifetime and bump allocation](#chapter-4--object-lifetime-and-bump-allocation)
5. [Free lists and individual reclamation](#chapter-5--free-lists-and-individual-reclamation)
6. [Immix and large-object spaces](#chapter-6--immix-and-large-object-spaces)
7. [Roots, tracing, and object graphs](#chapter-7--roots-tracing-and-object-graphs)
8. [Collection plans and statistics](#chapter-8--collection-plans-and-statistics)
9. [Write barriers and remembered information](#chapter-9--write-barriers-and-remembered-information)
10. [Work queues and phase pipelines](#chapter-10--work-queues-and-phase-pipelines)
11. [Mutators and safepoint coordination](#chapter-11--mutators-and-safepoint-coordination)
12. [The DSL toolkit in C](#chapter-12--the-dsl-toolkit-in-c)
13. [Plugins and custom extensions](#chapter-13--plugins-and-custom-extensions)
14. [The C++ wrapper](#chapter-14--the-c-wrapper)
15. [Testing, performance, and maintenance](#chapter-15--testing-performance-and-maintenance)

Each fenced C example is a complete translation unit. Build the library
once, save the example as `example.c`, then compile from the repository
root:

```sh
cmake -S . -B build && cmake --build build
cc -std=c99 -Iinclude example.c build/libmemtkx.a -pthread -o example
./example
```

Examples check their results with `assert`, so compile them without
`NDEBUG`. C++ examples (chapter 14) build the same way with
`c++ -std=c++17` (or `-std=c++20` where noted).

## Chapter 1 — Identity and architecture

### 1.1 The public identity

| Surface | Spelling |
| --- | --- |
| CMake project and package | `MemTKX` |
| Library file | `libmemtkx.a` / `libmemtkx.so` / `memtkx.lib` |
| C target | `MemTKX::MemTKX` |
| C++ wrapper target | `MemTKX::MemTKXCpp` |
| C prefix | `mtkx_` for functions and types; `MTKX_` for macros and enum constants |
| C umbrella headers | `<memtkx/memtkx.h>`, `<memtkx/dsl.h>` |
| C++ umbrella headers | `<memtkx/MemTKX.hpp>`, `<memtkx/MemTKXDSL.hpp>` |
| C++ namespaces | `MemTKX`, `MemTKX::dsl` |
| Options | `MemTKX_BUILD_TESTS`, `MemTKX_BUILD_BENCHMARKS`, `MemTKX_BUILD_EXAMPLES`, `MemTKX_ENABLE_WARNINGS`, `MemTKX_BUILD_CXX_WRAPPER`, `MemTKX_INSTALL` |

Every public C header can be included from C++ (declarations are wrapped
in `extern "C"`). Names that begin with `mtkx__` (double underscore) or
end with `_` are internal.

### 1.2 Five cooperating layers

1. **Core** (`memtkx/core/`): addresses, ranges, units, status codes,
   result types, address vectors, panic and allocator hooks.
2. **Allocators and spaces** (`memtkx/space/`) reserve bytes within a
   region. Some own their backing memory; others borrow it.
3. **Plans** (`memtkx/plan/`) track allocations and implement reachability
   or copying logic.
4. **Barriers** (`memtkx/barrier/`) record information about reference
   writes.
5. **Scheduling and coordination** (`memtkx/scheduler/`) organise work and
   track mutator state across threads.

A sixth, independent layer is the **DSL toolkit** (`memtkx/dsl/`):
macro-generated result and optional types, pipelines, predicates,
dispatch tables, ASTs with rewrite rules, expression trees, literal
tables, memoisation, parser combinators, PEG grammars, and task chains.

### 1.3 Conventions you will see everywhere

* **Objects you place yourself.** Allocators, spaces, card tables, queues
  and pipelines are structs with `*_init` and (when they own memory)
  `*_destroy`. Their fields are visible for inspection; change them only
  through functions.
* **Opaque objects.** Plans, the coordinator, PEG definitions and
  matchers, AST nodes and parsers are created with `*_create` / `*_new`
  and released with `*_destroy` / `*_free` / `*_release`.
* **Callbacks** are a function pointer plus a `void *ctx`.
* **Errors are values.** Allocation returns `mtkx_alloc_result`;
  collection returns `mtkx_plan_stats_result`; coordination returns
  `mtkx_gc_result`; everything else returns `mtkx_status`. Nothing in
  MemTKX uses `setjmp`, `errno`, or global error state.
* **Native-heap failures are reported, not fatal.** Bookkeeping
  allocation failures surface as `MTKX_ERR_NOMEM`,
  `MTKX_ALLOC_ALLOCATION_FAILED`, or `MTKX_GC_PLAN_UNAVAILABLE`.

### 1.4 What the embedding runtime supplies

A runtime decides what an object looks like, where its references live,
which values are roots, and how threads become quiescent. MemTKX cannot
infer any of this. The trace callback receives an object address and
appends the addresses of the objects it references.

Collection is explicit. An out-of-memory allocation result does not
invoke a collector, and plans do not scan native stacks. The coordinator
calls a stack scanner only if you registered one.

**Learning task:** find `mtkx_space_ops`, `mtkx_plan_ops`, and
`mtkx_trace_fn` in the headers, and identify which callback your runtime
would supply for each one.

## Chapter 2 — Building and integrating

### 2.1 Requirements and a first build

You need a C99 compiler and CMake 3.20 or later. Threads come from POSIX
threads (Linux, BSD, macOS) or the Win32 API. The C++ wrapper, which is on
by default, needs a C++17 compiler; turn it off with
`-DMemTKX_BUILD_CXX_WRAPPER=OFF` in C-only projects.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For multi-configuration generators, also pass `--config Debug` to the
build and `-C Debug` to CTest.

### 2.2 Source-tree integration

```cmake
cmake_minimum_required(VERSION 3.20)
project(ExampleRuntime LANGUAGES C)

set(MemTKX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MemTKX_BUILD_CXX_WRAPPER OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/memtkx)

add_executable(example_runtime main.c)
target_link_libraries(example_runtime PRIVATE MemTKX::MemTKX)
```

Set options before `add_subdirectory`. `MemTKX::MemTKX` carries the
include directory, the `c_std_99` requirement, `MTKX_SHARED` when the
library is shared, and the thread dependency.

### 2.3 Installation

```sh
cmake -S . -B build-install -DMemTKX_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-install --parallel
cmake --install build-install
```

| Setting | Installed content |
| --- | --- |
| `CMAKE_INSTALL_LIBDIR` | `libmemtkx`, `cmake/MemTKX/`, `pkgconfig/memtkx.pc` |
| `CMAKE_INSTALL_INCLUDEDIR` | `memtkx/` C headers, and C++ headers when the wrapper is enabled |
| `CMAKE_INSTALL_DOCDIR` | `README.md` and `GUIDE.md` |

### 2.4 Consuming the installed package

```cmake
cmake_minimum_required(VERSION 3.20)
project(InstalledExample LANGUAGES C)

find_package(MemTKX 2 CONFIG REQUIRED)
add_executable(installed_example main.c)
target_link_libraries(installed_example PRIVATE MemTKX::MemTKX)
```

Configure with `-DCMAKE_PREFIX_PATH="$HOME/.local"`, or point `MemTKX_DIR`
at the directory containing `MemTKXConfig.cmake`. The package defines the
components `C` (always) and `Cpp` (when the wrapper was installed), so
`find_package(MemTKX 2 CONFIG REQUIRED COMPONENTS Cpp)` fails early if
the wrapper is missing. Version compatibility is `SameMajorVersion`.

Without CMake, use pkg-config:

```sh
cc -std=c99 main.c $(pkg-config --cflags --libs memtkx) -pthread
```

### 2.5 Static and shared builds

The library is static by default. Configure with `-DBUILD_SHARED_LIBS=ON`
for a shared library with the soname `libmemtkx.so.2`. Shared builds
export only the `MTKX_API` functions; everything else is hidden. When
compiling against a shared MemTKX without CMake on Windows, define
`MTKX_SHARED` so declarations use `__declspec(dllimport)`.

### 2.6 Include granularity

Use `<memtkx/memtkx.h>` while learning. Later you can include just what a
component needs, such as `<memtkx/space/bump_pointer.h>` or
`<memtkx/barrier/card_table.h>`. The DSL headers do not depend on the
memory-management headers.

**Learning task:** install MemTKX, then build a separate consumer with
`find_package`. This tests package discovery rather than source-tree
includes.

## Chapter 3 — Addresses, units, and results

### 3.1 Address vocabulary

`mtkx_address`, `mtkx_object_reference`, and `mtkx_word` are `uintptr_t`.
`mtkx_byte` is `unsigned char` and `mtkx_offset` is `ptrdiff_t`.
`MTKX_WORD_BYTES` is the size of a word.

`mtkx_as_address(ptr)` turns a pointer into an address.
`mtkx_from_address(T, addr)` turns an address back into a `T *`. Neither
allocates, initialises, or checks anything.

`MTKX_ALIGNOF(T)` is the alignment of a type: `alignof` in C++,
`_Alignof` in C11, and a portable `sizeof` probe in C99. `MTKX_MAX_ALIGN`
is the fundamental alignment, the C99 equivalent of
`alignof(max_align_t)`.

### 3.2 Regions are half-open

`mtkx_address_range` describes `[start, end)`; build one with
`mtkx_range(start, end)`. Empty or reversed ranges have size zero. The
helpers are `mtkx_range_contains`, `mtkx_range_contains_range`,
`mtkx_range_overlaps`, `mtkx_range_clamp`, `mtkx_range_size`,
`mtkx_range_empty`, and `mtkx_range_equal`. Clamping an address above the
region returns `end`, which is an exclusive boundary.

### 3.3 Alignment and sizes

Pass power-of-two alignments. `mtkx_is_power_of_two` rejects zero.
`mtkx_align_up` and `mtkx_align_down` use bit masks and do not validate
the alignment; allocator entry points do their own checks.
`mtkx_can_allocate(start, limit, bytes, alignment)` tests whether a request
fits.

### 3.4 Memory quantities

| Macro | Meaning |
| --- | --- |
| `MTKX_B(64)` | 64 bytes |
| `MTKX_KB(4)` | 4 × 1024 bytes |
| `MTKX_MB(8)` | 8 × 1024² bytes |
| `MTKX_GB(1)` | 1024³ bytes |

These are integer constant expressions, so they can size arrays and
appear in static assertions. Scaling is binary despite the short names.
`mtkx_bytes_to_kib`, `mtkx_kib_to_bytes`, `mtkx_bytes_to_mib`, and
`mtkx_mib_to_bytes` convert using integer division.

`mtkx_units_parse_literal("1.5_MB", &out)` parses runtime strings. It
returns `MTKX_ERR_NOT_FOUND` for an unknown suffix, `MTKX_ERR_PARSE` for a
missing number, and `MTKX_ERR_RANGE` on overflow. Fractional bytes are
truncated.

### 3.5 Explicit result handling

Allocation returns `mtkx_alloc_result`: `{bool ok; mtkx_address value;
mtkx_alloc_error error;}`. Collections return `mtkx_plan_stats_result`,
and coordination and phases return `mtkx_gc_result` (an `int` value). All
of them are stamped out by `MTKX_DEFINE_RESULT`, which you can use for
your own types too (chapter 12):

| Function (for result type `R`) | Purpose |
| --- | --- |
| `R_is_ok(r)` / `R_is_err(r)` | Inspect the active branch |
| `R_unwrap(r)` | The value; **panics** on an error (§3.7) |
| `R_unwrap_or(r, fallback)` | The value or a fallback |
| `R_unwrap_err(r)` | The error; panics on success |
| `R_get(r, &out)` / `R_get_err(r, &out)` | Non-panicking access |
| `R_map(r, fn, ctx)` | Transform a success value (same type) |
| `R_map_err(r, fn, ctx)` | Transform an error (same type) |
| `R_and_then(r, fn, ctx)` | Chain another result-producing step |
| `R_from_ok(v)` / `R_from_err(e)` | Construct a branch |

The fields are public, so `r.ok`, `r.value`, and `r.error` work
directly. This example exercises both branches:

```c
#include <memtkx/memtkx.h>

#include <assert.h>
#include <stdio.h>

typedef char kb_check[MTKX_KB(2) == 2048 ? 1 : -1]; /* compile-time */

static int twice(int v, void *ctx) {
  (void)ctx;
  return v * 2;
}

static mtkx_alloc_error report(mtkx_alloc_error e, void *ctx) {
  *(int *)ctx = 1;
  printf("%s\n", mtkx_alloc_error_string(e));
  return e;
}

int main(void) {
  mtkx_address_range range = mtkx_range(100, 200);
  mtkx_gc_result value = mtkx_gc_result_from_ok(21);
  mtkx_alloc_result failure = mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
  int observed = 0;

  assert(mtkx_align_up(17, 8) == 24);
  assert(mtkx_range_contains(range, 100) && !mtkx_range_contains(range, 200));
  assert(mtkx_gc_result_unwrap(mtkx_gc_result_map(value, twice, NULL)) == 42);

  (void)mtkx_alloc_result_map_err(failure, report, &observed);
  assert(observed && mtkx_alloc_result_unwrap_or(failure, 0) == 0);
  return 0;
}
```

### 3.6 Error vocabulary

Allocation errors are `MTKX_ALLOC_OUT_OF_MEMORY`,
`MTKX_ALLOC_MISALIGNED`, `MTKX_ALLOC_LARGE_OBJECT_EXCEEDED`, and
`MTKX_ALLOC_ALLOCATION_FAILED`. The last also covers zero-byte requests
and failed bookkeeping.

Collection errors are `MTKX_GC_MUTATOR_NOT_REGISTERED`,
`MTKX_GC_SAFEPOINT_TIMEOUT`, `MTKX_GC_PLAN_UNAVAILABLE` (failed promotion,
or a plan whose bookkeeping ran out of memory), `MTKX_GC_INVALID_TRACE` (a
trace callback returned `false`), and `MTKX_GC_COORDINATOR_BUSY`.

`mtkx_alloc_error_string`, `mtkx_gc_error_string`, and
`mtkx_status_string` return static text.

### 3.7 Panics and native-heap hooks

Misuse that cannot be returned as a value, such as unwrapping an error,
calls `mtkx_panic(message)`. By default that prints to `stderr` and calls
`abort()`. `mtkx_set_panic_handler` installs your own handler (logging, a
`longjmp` to a recovery point, an exception bridge). A handler must not
return; if it does, `abort()` runs anyway.

`mtkx_set_allocator(&hooks)` routes MemTKX's own bookkeeping memory
through your `malloc`/`calloc`/`realloc`/`free`. That covers object
tables, free-cell arrays, names, owned heap buffers, AST nodes, and
parser values. Install the hooks before creating any MemTKX object.
Release strings and dumps returned by MemTKX with `mtkx_free`.

## Chapter 4 — Object lifetime and bump allocation

### 4.1 Reserve bytes, then initialise objects

An allocator returns an address, not an initialised object. Write the
object's fields before anything reads them, including a trace callback.
Collectors never run finalisers. A runtime whose objects own other
resources must release those itself.

### 4.2 Borrowed storage has a stable lifetime

Bump allocators borrow a region. A static array, a `malloc` block, or a
`mmap` mapping all work, as long as the memory outlives the allocator and
does not move. Never let two allocators manage the same region at once
unless you have partitioned it explicitly.

### 4.3 A complete arena example

```c
#include <memtkx/memtkx.h>

#include <assert.h>
#include <string.h>

typedef struct message {
  char text[32];
} message;

static double storage[4096 / sizeof(double)];

int main(void) {
  const mtkx_address start = mtkx_as_address(storage);
  mtkx_bump_allocator arena;
  mtkx_alloc_result allocation;
  message *m;

  mtkx_bump_allocator_init(&arena, start, start + sizeof storage);
  allocation = mtkx_bump_allocator_allocate(&arena, sizeof(message), MTKX_ALIGNOF(message));
  assert(mtkx_alloc_result_is_ok(allocation));
  m = mtkx_from_address(message, allocation.value);
  strcpy(m->text, "hello");
  assert(strcmp(m->text, "hello") == 0);
  assert(mtkx_bump_allocator_allocation_count(&arena) == 1);
  assert(mtkx_bump_allocator_used_bytes(&arena) >= sizeof(message));

  mtkx_bump_allocator_reset(&arena);
  assert(mtkx_bump_allocator_used_bytes(&arena) == 0);
  assert(mtkx_bump_allocator_allocation_count(&arena) == 1);
  return 0;
}
```

### 4.4 Cursor behavior and accounting

Initialisation aligns the start up and the end down to `MTKX_MAX_ALIGN`.
Each request aligns the cursor to its own alignment, and that padding
uses up capacity.

| Function | Meaning |
| --- | --- |
| `mtkx_bump_allocator_start/limit/cursor` | Boundaries and cursor |
| `mtkx_bump_allocator_remaining` | Bytes after the cursor |
| `mtkx_bump_allocator_used_bytes` | Cursor distance from start, including padding |
| `mtkx_bump_allocator_allocated_bytes` | Sum of successful requested sizes |
| `mtkx_bump_allocator_allocation_count` | Number of successful requests |
| `mtkx_bump_allocator_reset` | Return the cursor to its start |
| `mtkx_bump_allocator_reset_region` | Replace the boundaries and reset the cursor |

Neither kind of reset clears the cumulative counters, zeroes memory, or
finalises objects.

### 4.5 The inline fast path

`mtkx_bump_allocator_allocate` is an exported function, which suits
language bindings and shared libraries. For hot allocation paths, the
header also defines `mtkx_bump_allocator_try_allocate`, a `static inline`
version. It returns the address, or `0` on any failure, and updates the
allocator exactly like the full call. When you need to know why it
failed, call the full function; a failed request changes nothing, so the
retry is safe. Chapter 15 compares the two.

### 4.6 The bump space adds a descriptor

`mtkx_bump_space_init(&space, name, start, end)` wraps the allocator with
space metadata: a name, a kind, a region, a default alignment, and
`mtkx_space_metrics`. Allocate with `mtkx_bump_space_allocate` or with the
generic `mtkx_space_allocate(&space.base, ...)`, and release the name with
`mtkx_bump_space_destroy`. `mtkx_bump_space_reset` resets the cursor but
keeps the metrics.

**Learning task:** allocate objects with alignments 8 and 64, then compare
`used_bytes` with `allocated_bytes`. The difference is alignment overhead.

## Chapter 5 — Free lists and individual reclamation

### 5.1 Free-list allocation

`mtkx_free_list_allocator_init(&fl, start, end)` starts with one free cell
covering the region. Allocation is first-fit in address order. Prefix
padding and any remaining suffix go back on the list as free cells.
Freeing inserts the cell in order and merges it with adjacent cells. Cells
live in a sorted array, so the C version replaces only the affected cells
instead of re-sorting on every operation.

### 5.2 Free the exact allocation

`mtkx_free_list_allocator_free(&fl, address, bytes)` needs the address and
requested size that allocation returned. It checks region bounds but
cannot detect double frees or wrong sizes.

```c
#include <memtkx/memtkx.h>

#include <assert.h>

static double storage[4096 / sizeof(double)];

int main(void) {
  const mtkx_address start = mtkx_as_address(storage);
  mtkx_free_list_space heap;
  mtkx_alloc_result first, second;
  bool freed_first, freed_second;

  if (mtkx_free_list_space_init(&heap, "objects", start, start + sizeof storage) != MTKX_OK) {
    return 1;
  }
  first = mtkx_free_list_space_allocate(&heap, 128, 16);
  second = mtkx_free_list_space_allocate(&heap, 256, 16);
  assert(first.ok && second.ok);
  assert(mtkx_space_metrics_of(&heap.base)->live_bytes == 384);

  freed_first = mtkx_free_list_space_deallocate(&heap, first.value, 128);
  freed_second = mtkx_free_list_space_deallocate(&heap, second.value, 256);
  assert(freed_first && freed_second);
  assert(mtkx_space_metrics_of(&heap.base)->live_bytes == 0);
  assert(mtkx_free_list_allocator_free_bytes(&heap.allocator) == sizeof storage);
  assert(mtkx_free_list_allocator_free_cells(&heap.allocator) == 1);
  mtkx_free_list_space_destroy(&heap);
  return 0;
}
```

### 5.3 Metrics and reset

`mtkx_free_list_allocator_free_bytes` sums the free cells. A large total
does not guarantee that one large request fits, because the space may be
fragmented across cells; `free_cells` shows how fragmented it is.
`mtkx_free_list_allocator_reset` clears the cells and counters.
`mtkx_free_list_space_reset` resets the allocator to the space's region
but keeps `mtkx_space_metrics`. Metrics count requested bytes, not
padding.

### 5.4 Segregated free-list allocation

`mtkx_segregated_allocator` splits its region into eight independent
partitions. The class thresholds are in `mtkx_segregated_size_classes`:

```text
16, 32, 64, 128, 256, 512, 1024, 4096 bytes
```

A request tries the first class large enough for it, then the larger
ones. The first seven partitions are equal in size and the last gets the
remainder. Requests above 4096 bytes fail with
`MTKX_ALLOC_OUT_OF_MEMORY`. Free finds the partition by address, so a
small request that fell back to a larger class still frees correctly. Call
`mtkx_segregated_allocator_destroy` when you are done.

### 5.5 Choosing a mechanism

Use a bump arena when every allocation shares one lifetime. Use a free
list when lifetimes differ and individual reuse matters. Use segregated
partitions when your request sizes follow a predictable distribution.

**Learning task:** allocate three cells, free the first and third, and
request their combined size. Then free the middle cell and retry to see
coalescing. (`tests/c/test_allocator.c` contains a solution.)

## Chapter 6 — Immix and large-object spaces

### 6.1 Immix layout

`mtkx_immix_space_init(&space, bytes)` allocates its own backing storage
and builds fixed-size blocks:

| Constant | Value |
| --- | --- |
| `MTKX_IMMIX_BLOCK_BYTES` | 32 KiB |
| `MTKX_IMMIX_LINE_BYTES` | 256 bytes |
| `MTKX_IMMIX_LINES_PER_BLOCK` | 128 |

Storage is rounded up to whole blocks and padded for line alignment.
`mtkx_range_size(mtkx_space_region(&space.base))` covers the usable
blocks; `mtkx_immix_space_total_bytes` is the whole owned allocation.
Blocks allocate with a forward cursor and mark the lines each request
touches. A request larger than one block fails with
`MTKX_ALLOC_LARGE_OBJECT_EXCEEDED`. Inspect individual blocks with
`mtkx_immix_space_block(&space, i)` and the `mtkx_immix_block_*`
accessors.

### 6.2 Marks describe lines

`mtkx_immix_space_clear_marks` clears line marks without moving cursors.
`mtkx_immix_space_mark(&space, address, bytes)` marks the lines of a known
object. `mtkx_immix_space_marked_bytes` is marked lines × 256, not the sum
of object sizes. `mtkx_immix_space_reset_blocks` resets every cursor and
mark. `mtkx_immix_space_destroy` releases the storage.

### 6.3 A marking experiment

```c
#include <memtkx/memtkx.h>

#include <assert.h>

int main(void) {
  mtkx_immix_space space;
  mtkx_alloc_result object;
  bool marked;

  if (mtkx_immix_space_init(&space, MTKX_IMMIX_BLOCK_BYTES * 2) != MTKX_OK) {
    return 1;
  }
  object = mtkx_immix_space_allocate(&space, 300, MTKX_IMMIX_LINE_BYTES);
  assert(object.ok);
  assert(mtkx_space_contains(&space.base, object.value));
  assert(mtkx_immix_space_block_count(&space) >= 2);

  mtkx_immix_space_clear_marks(&space);
  assert(mtkx_immix_space_marked_bytes(&space) == 0);
  marked = mtkx_immix_space_mark(&space, object.value, 300);
  assert(marked);
  assert(mtkx_immix_space_marked_bytes(&space) == 2 * MTKX_IMMIX_LINE_BYTES);

  mtkx_immix_space_reset_blocks(&space);
  assert(mtkx_immix_space_used_bytes(&space) == 0);
  mtkx_immix_space_destroy(&space);
  return 0;
}
```

The 300-byte object starts on a line boundary and touches two lines.

### 6.4 The large-object space

`mtkx_los_space_init(&los, name, start, end, threshold)` borrows a region
and bump-allocates inside it. Pass `MTKX_DEFAULT_LARGE_OBJECT_THRESHOLD`
(4096) for the usual threshold. A request below the threshold, or larger
than the whole region, fails with `MTKX_ALLOC_LARGE_OBJECT_EXCEEDED`. An
accepted request can still fail with `MTKX_ALLOC_OUT_OF_MEMORY`.

```c
#include <memtkx/memtkx.h>

#include <assert.h>

static double storage[16384 / sizeof(double)];

int main(void) {
  const mtkx_address start = mtkx_as_address(storage);
  mtkx_los_space large;
  mtkx_alloc_result object, too_small;
  size_t capacity_after_allocation;
  bool accounted;

  if (mtkx_los_space_init(&large, "large", start, start + sizeof storage,
                          MTKX_DEFAULT_LARGE_OBJECT_THRESHOLD) != MTKX_OK) {
    return 1;
  }
  too_small = mtkx_los_space_allocate(&large, 128, 16);
  assert(mtkx_alloc_result_is_err(too_small));

  object = mtkx_los_space_allocate(&large, 8192, 16);
  assert(object.ok);
  capacity_after_allocation = mtkx_bump_allocator_remaining(&large.allocator);
  accounted = mtkx_los_space_deallocate(&large, object.value, 8192);
  assert(accounted);
  assert(mtkx_space_metrics_of(&large.base)->live_bytes == 0);
  assert(mtkx_bump_allocator_remaining(&large.allocator) == capacity_after_allocation);

  mtkx_los_space_reset(&large);
  assert(mtkx_bump_allocator_used_bytes(&large.allocator) == 0);
  mtkx_los_space_destroy(&large);
  return 0;
}
```

Freeing a single object only updates the accounting; the space becomes
reusable when you reset the whole space.

### 6.5 Routing small and large requests

You can route requests by size to separate spaces, for example a free
list for small objects and a large-object space for anything over 4 KiB.
The built-in plans do not route for you. If you add routing, your trace
and root logic must know which manager owns each object.

## Chapter 7 — Roots, tracing, and object graphs

### 7.1 Reachability is a runtime contract

A root is an object address that must stay reachable on its own. A trace
callback has this type:

```text
bool (*mtkx_trace_fn)(mtkx_address object, mtkx_address_vec *children, void *ctx);
```

It appends the objects that `object` references, using
`mtkx_address_vec_push` or `_append`. It returns `false` if the object
cannot be scanned, including when an append fails; the collection then
stops with `MTKX_GC_INVALID_TRACE`. Wrap the callback with
`mtkx_trace_make(fn, ctx)`. `mtkx_trace_none()` reports no children.

Plans recognise objects by their start address in a tracking table.
Interior pointers do not keep their containing object alive.

### 7.2 A complete two-node graph

```c
#include <memtkx/memtkx.h>

#include <assert.h>
#include <stdlib.h>

typedef struct node {
  mtkx_address next;
  int value;
} node;

static bool trace(mtkx_address address, mtkx_address_vec *children, void *ctx) {
  const node *n = mtkx_from_address(const node, address);
  (void)ctx;
  return n->next == 0 || mtkx_address_vec_push(children, n->next) == MTKX_OK;
}

static mtkx_address make_node(mtkx_mark_sweep_plan *heap, int value) {
  mtkx_alloc_result a = mtkx_mark_sweep_plan_allocate(heap, sizeof(node), MTKX_ALIGNOF(node));
  node *n;
  if (!a.ok) {
    abort();
  }
  n = mtkx_from_address(node, a.value);
  n->next = 0;
  n->value = value;
  return a.value;
}

int main(void) {
  mtkx_mark_sweep_plan *heap = mtkx_mark_sweep_plan_create(4096);
  const mtkx_trace t = mtkx_trace_make(trace, NULL);
  mtkx_address root, child;
  mtkx_plan_stats_result r;

  root = make_node(heap, 10);
  child = make_node(heap, 20);
  (void)make_node(heap, 30);
  mtkx_from_address(node, root)->next = child;

  r = mtkx_mark_sweep_plan_collect(heap, &root, 1, t);
  assert(r.ok && r.value.live_objects == 2 && r.value.freed_objects == 1);
  assert(mtkx_from_address(node, child)->value == 20);

  mtkx_from_address(node, root)->next = 0;
  r = mtkx_mark_sweep_plan_collect(heap, &root, 1, t);
  assert(r.ok && r.value.live_objects == 1 && r.value.freed_objects == 1);

  r = mtkx_mark_sweep_plan_collect(heap, NULL, 0, t);
  assert(r.ok && mtkx_mark_sweep_plan_live_objects(heap) == 0);
  mtkx_mark_sweep_plan_destroy(heap);
  return 0;
}
```

A local variable named `child` does not keep its object alive. Only
addresses passed as roots, or reached from them, survive.

### 7.3 Cycles and duplicate references

Mark-sweep keeps a visited set, so cycles terminate and duplicate edges
are scanned once. A cycle with no path from a root is unreachable and
gets freed. For tagged object layouts, read the header in the callback
and switch on the layout. Report only managed references.

### 7.4 Allocation failure and explicit retry

```text
try allocation
if it fails because of capacity:
    publish current roots and pause mutation
    run collection
    retry allocation once
if it still fails:
    return the allocation failure to the application
```

Keep a new object out of tracing until its reference fields are
initialised. Make sure objects under construction are roots if
construction can trigger a collection.

### 7.5 Trace callback discipline

Treat scanning as a read-only pass over a quiescent graph. Do not
allocate into the plan that is collecting. The `children` vector belongs
to the plan, so only append to it.

**Learning task:** change the example into a two-node cycle. Collect with
one root, then with no roots, and compare the live and freed counts.
`tests/c/test_plans.c` contains a solution.

## Chapter 8 — Collection plans and statistics

### 8.1 The common plan interface

Each plan is an opaque object with `*_create`, `*_destroy`, `*_allocate`,
and `*_collect`. Each also converts to the polymorphic base:

```text
mtkx_plan *p = mtkx_mark_sweep_plan_as_plan(heap);
mtkx_plan_get_kind(p);       /* MTKX_PLAN_MARK_SWEEP              */
mtkx_plan_name(p);           /* "MarkSweep"                       */
mtkx_plan_allocate(p, bytes, alignment);
mtkx_plan_collect(p, roots, root_count, trace);
mtkx_plan_destroy(p);        /* destroys the concrete plan        */
```

The kinds are `MTKX_PLAN_MARK_SWEEP`, `MTKX_PLAN_SEMI_SPACE`,
`MTKX_PLAN_IMMIX`, and `MTKX_PLAN_GENERATIONAL`; `mtkx_plan_kind_string`
gives a short label for each. A collection returns
`mtkx_plan_stats_result` with these statistics:

| Field | Meaning |
| --- | --- |
| `live_objects` | Reachable or surviving objects |
| `live_bytes` | Sum of their requested sizes |
| `freed_objects` | Objects reported as freed (mark-sweep only) |
| `freed_bytes` | Requested bytes reported as freed (mark-sweep only) |
| `collection_count` | Successful collections so far |

### 8.2 Mark-sweep

`mtkx_mark_sweep_plan_create(heap_bytes)` owns a zeroed heap and a free
list. Collection marks from the roots, then returns every unmarked object
to the free list. Survivors keep their addresses, so you can collect
repeatedly over the same graph.

`mtkx_mark_sweep_plan_live_objects` and `_live_bytes` describe the
currently tracked allocations, and `_contains_object` tests whether an
address is a tracked object start. `_mark` and `_is_marked` let you
inspect marks. A collection clears marks before marking and again after
sweeping, so pre-marking an object does not preserve it.

### 8.3 Semi-space

`mtkx_semi_space_plan_create(bytes)` owns two equal halves. A collection
copies reachable tracked objects into to-space with `memcpy`, then swaps
the halves. It neither rewrites roots or fields nor exposes forwarding
addresses, so do not carry old addresses into another collection. Copies
are aligned to `MTKX_MAX_ALIGN`. If to-space runs out, the remaining
objects are dropped without an error, as in the original C++
implementation. Treat this plan as a single-cycle experiment.

```c
#include <memtkx/memtkx.h>

#include <assert.h>
#include <string.h>

int main(void) {
  mtkx_semi_space_plan *heap = mtkx_semi_space_plan_create(4096);
  mtkx_alloc_result allocated = mtkx_semi_space_plan_allocate(heap, 64, MTKX_MAX_ALIGN);
  mtkx_plan_stats_result collected;
  assert(allocated.ok);
  memset(mtkx_from_address(void, allocated.value), 0, 64);

  collected = mtkx_semi_space_plan_collect(heap, &allocated.value, 1, mtkx_trace_none());
  assert(collected.ok);
  assert(collected.value.live_objects == 1 && collected.value.live_bytes == 64);
  /* the old address is not used after copying */
  mtkx_semi_space_plan_destroy(heap);
  return 0;
}
```

### 8.4 Immix

`mtkx_immix_plan_create(bytes)` owns an Immix space and tracks allocation
sizes. A collection clears earlier marks, then marks the lines of every
reachable object. It does not sweep, remove dead table entries, or reset
cursors. `mtkx_immix_plan_space(plan)` returns the space for observation.
Its `used_bytes`, `marked_bytes`, and the stats' `live_bytes` each answer
a different question, which makes the plan useful for studying line
fragmentation.

### 8.5 Generational

`mtkx_generational_plan_create(nursery, mature)` owns two heaps and a card
table over the mature heap. New objects bump-allocate in the nursery.
`mtkx_generational_plan_minor_collect` (or `_collect`) copies reachable
tracked objects into the mature free list and replaces the object table
with the copies. It then resets the nursery and clears the cards.
`mtkx_generational_plan_remember(plan, slot_address)` dirties a card, and
`_dirty_cards` counts dirty cards. Minor collection does not scan cards
as roots.

A failed promotion returns `MTKX_GC_PLAN_UNAVAILABLE` and does not roll
back earlier copies. `mtkx_generational_plan_nursery_live_bytes` sums the
tracked bytes that still lie in the nursery.

### 8.6 Plan selection in practice

| Goal | Starting point |
| --- | --- |
| Non-moving graph with reclaimed unreachable objects | Mark-sweep |
| Batch-lifetime objects | A bump allocator without a plan |
| Study forwarding and copying | Semi-space, single-cycle experiments |
| Measure reachable line occupancy | Immix |
| Study nursery-to-mature copying | Generational |

A full moving runtime also needs a writable-slot trace interface or
handles, an observable relocation protocol, alignment tracking, and
defined failure handling.

## Chapter 9 — Write barriers and remembered information

### 9.1 Why record writes?

A barrier records what a collector policy needs to know when a reference
changes. An old-to-young store may need a remembered slot, and a
concurrent snapshot needs the overwritten reference.

`mtkx_slot` holds `mtkx_address *slot_address` (the field being written)
and `mtkx_address target_object` (the object it will refer to); build one
with `mtkx_slot_make`. `mtkx_read_slot` describes a load.

### 9.2 Barrier stages

A barrier pipeline is an array of `mtkx_barrier_stage` values that
`mtkx_barrier_run(slot, stages, count)` applies in order. When a filter
rejects the slot, the remaining filters are skipped and sinks receive
nothing. Unlike the C++ stages, any order of stages is valid.

| Constructor | Effect |
| --- | --- |
| `mtkx_barrier_non_null()` | Keep slots with a location and a nonzero target |
| `mtkx_barrier_filter_range(r)` | Keep slots whose *location* lies in `r` |
| `mtkx_barrier_filter_target_range(r)` | Keep slots whose *target* lies in `r` |
| `mtkx_barrier_cross_region(src, tgt)` | Location in `src` and target in `tgt` |
| `mtkx_barrier_saturating_filter(src, tgt)` | Same predicate as `cross_region` |
| `mtkx_barrier_record(&buffer)` | Append the surviving slot to an `mtkx_slot_buffer` |
| `mtkx_barrier_discard()` | Drop it |
| `mtkx_barrier_custom(fn, ctx)` | Your stage; it may edit the slot, and returning `false` drops it |

`mtkx_run_write_barrier(slot, source, target, &buffer)` is the common
two-stage pipeline: the saturating filter, then record. Like every stage,
it records but never performs the store.

```c
#include <memtkx/memtkx.h>

#include <assert.h>

int main(void) {
  mtkx_address field = 0;
  const mtkx_address field_address = mtkx_as_address(&field);
  const mtkx_address_range source = mtkx_range(field_address, field_address + sizeof field);
  const mtkx_address_range target = mtkx_range(0x1000, 0x3000);
  mtkx_slot_buffer modified;

  mtkx_slot_buffer_init(&modified);
  assert(mtkx_run_write_barrier(mtkx_slot_make(&field, 0x2000), source, target, &modified));
  assert(field == 0);
  field = 0x2000; /* the store itself */
  assert(modified.size == 1 && modified.data[0].slot_address == &field);
  mtkx_slot_buffer_destroy(&modified);
  return 0;
}
```

If a slot buffer cannot grow, the record stage drops the slot and
increments the buffer's `dropped` counter.

### 9.3 Card tables

`mtkx_card_table_init(&cards, base, region_bytes, card_bytes)` maps
addresses to one byte per card; pass `MTKX_DEFAULT_CARD_BYTES` (512) for
the usual card size. The functions are `mark`, `clear`, `clear_all`,
`is_marked`, `marked_cards`, and `card_index`. Addresses outside the
region are ignored, and a zero card size creates a table with no cards.
`mtkx_card_table_barrier_apply(&barrier, slot, value)` dirties the slot's
card and does not store `value`.

### 9.4 SATB captures the old reference

`mtkx_satb_barrier_capture(&b, slot)` reads the slot and queues the value
if it is nonzero. `mtkx_satb_barrier_write(&b, slot, value)` captures and
then stores. `mtkx_satb_queue_take_entries(&q, &out)` moves every entry
into an address vector and leaves the queue empty.

```c
#include <memtkx/memtkx.h>

#include <assert.h>

int main(void) {
  mtkx_address field = 77;
  const mtkx_address address = mtkx_as_address(&field);
  mtkx_card_table cards;
  mtkx_card_table_barrier card_barrier;
  mtkx_satb_queue old_references;
  mtkx_satb_barrier satb;
  mtkx_address_vec captured = MTKX_ADDRESS_VEC_INIT;

  if (mtkx_card_table_init(&cards, address, sizeof field, MTKX_DEFAULT_CARD_BYTES) != MTKX_OK) {
    return 1;
  }
  card_barrier = mtkx_card_table_barrier_make(&cards);
  mtkx_satb_queue_init(&old_references);
  satb = mtkx_satb_barrier_make(&old_references);

  mtkx_card_table_barrier_apply(&card_barrier, &field, 88);
  assert(field == 77 && mtkx_card_table_is_marked(&cards, address));
  mtkx_satb_barrier_write(&satb, &field, 88);
  assert(field == 88);

  mtkx_satb_queue_take_entries(&old_references, &captured);
  assert(captured.size == 1 && captured.data[0] == 77);
  assert(mtkx_satb_queue_empty(&old_references));

  mtkx_address_vec_destroy(&captured);
  mtkx_satb_queue_destroy(&old_references);
  mtkx_card_table_destroy(&cards);
  return 0;
}
```

### 9.5 Connecting records to a policy

Your collector must consume modified slots, dirty cards, or SATB entries
at the right phase; the built-in plans do not connect them to their trace
loops. Use per-mutator buffers or external synchronisation, because the
buffers, queues, and card tables are not thread-safe.

## Chapter 10 — Work queues and phase pipelines

### 10.1 Work packets

`mtkx_work_packet` holds a `mtkx_work_kind`, an object address, and an
`extra` value. The kinds are `MTKX_WORK_ROOT_SCAN`, `_PROCESS_EDGE`,
`_MARK`, `_SWEEP`, `_RELEASE`, and `_USER`. `mtkx_work_queue` is a
ring-buffer FIFO with `push`, `try_pop`, `empty`, `size`, `clear`, and
`drain(fn, ctx)`. Draining handles packets until the queue is empty,
including packets the handler pushes, and returns the number it processed.

### 10.2 Collection phases

`mtkx_make_phase(name, fn, ctx, stop_on_error)` describes a phase whose
function takes `mtkx_phase_state *` and returns `mtkx_gc_result`. Append
phases with `mtkx_task_pipeline_append`, or append a whole pipeline with
`mtkx_task_pipeline_append_pipeline`; the pipeline copies each name.

`mtkx_phase_state` holds the running phase's name, a `completed_steps`
counter, and `elapsed_ns`, the duration of the most recent phase. A run
resets `completed_steps`, and phases increment it with
`mtkx_phase_state_note_completion`. A successful run returns the counter,
not the last phase's value. `phase_name` points into the pipeline, so it
stays valid only while the pipeline exists.

### 10.3 A queue-processing pipeline

```c
#include <memtkx/memtkx.h>

#include <assert.h>
#include <string.h>

static void check(const mtkx_work_packet *packet, void *ctx) {
  (void)ctx;
  assert(packet->object != 0);
}

static mtkx_gc_result scan(mtkx_phase_state *state, void *ctx) {
  size_t processed = mtkx_work_queue_drain((mtkx_work_queue *)ctx, check, NULL);
  mtkx_phase_state_note_completion(state, processed);
  return mtkx_gc_result_from_ok((int)processed);
}

static mtkx_gc_result report(mtkx_phase_state *state, void *ctx) {
  (void)ctx;
  assert(state->completed_steps == 2);
  return mtkx_gc_result_from_ok(0);
}

int main(void) {
  mtkx_work_queue queue;
  mtkx_task_pipeline pipeline;
  mtkx_phase_state state;
  mtkx_gc_result result;

  mtkx_work_queue_init(&queue);
  mtkx_work_queue_push(&queue, mtkx_work_packet_make(MTKX_WORK_ROOT_SCAN, 10, 0));
  mtkx_work_queue_push(&queue, mtkx_work_packet_make(MTKX_WORK_MARK, 20, 0));

  mtkx_task_pipeline_init(&pipeline);
  mtkx_task_pipeline_append(&pipeline, mtkx_make_phase("scan", scan, &queue, true));
  mtkx_task_pipeline_append(&pipeline, mtkx_make_phase("report", report, NULL, true));

  mtkx_phase_state_init(&state);
  result = mtkx_task_pipeline_run(&pipeline, &state);
  assert(result.ok && result.value == 2);
  assert(strcmp(state.phase_name, "report") == 0);
  assert(mtkx_task_pipeline_size(&pipeline) == 2 && mtkx_work_queue_empty(&queue));

  mtkx_task_pipeline_destroy(&pipeline);
  mtkx_work_queue_destroy(&queue);
  return 0;
}
```

### 10.4 Error and cleanup policy

A failing phase with `stop_on_error` ends the run with that phase's error.
When `stop_on_error` is false, the run continues and can still succeed
overall. Nothing logs ignored errors, and there is no automatic `finally`
phase. Release stopped mutators through a cleanup path that runs whether
or not the phases succeed.

### 10.5 Orchestration does not imply parallelism

Phases run one after another on the calling thread. `elapsed_ns` covers
only the last phase, so time the whole run yourself if you need the total.
The DSL's task chain (§12.10) is a separate API.

## Chapter 11 — Mutators and safepoint coordination

### 11.1 Thread records and root discovery

`mtkx_coordinator_create(expected_mutators)` returns a thread-safe
coordinator built on a mutex and a condition variable. Register mutators
with `mtkx_thread_record` values; `mtkx_thread_record_init(&r, id, name)`
fills in the defaults. A record can carry two callbacks:

```text
mtkx_status (*mtkx_root_provider_fn)(mtkx_address_vec *roots, void *ctx);
mtkx_status (*mtkx_stack_scan_fn)(mtkx_thread_id id, mtkx_address_vec *roots, void *ctx);
```

`mtkx_coordinator_register_mutator` copies the record, including its name.
It returns `MTKX_ERR_DUPLICATE` for an id that is already registered.
`_unregister_mutator` removes a record, and `_registered_mutators` counts
them. Asking for the status of an unknown id returns
`MTKX_MUTATOR_BLOCKED`.

### 11.2 Status is cooperative metadata

The statuses are `MTKX_MUTATOR_RUNNING`, `_POLLING`, `_AT_SAFEPOINT`, and
`_BLOCKED`. `mtkx_coordinator_update_status` changes a record and wakes
any waiters. A stop-the-world request succeeds once at least
`expected_mutators` records exist and every one of them is
`AT_SAFEPOINT`. `BLOCKED` does not count. Updating a status does not
pause a thread.

### 11.3 Stop, poll, and release

`mtkx_coordinator_request_stop_the_world(c, timeout_ms)` advances a
generation counter and then waits; `MTKX_DEFAULT_STOP_THE_WORLD_TIMEOUT_MS`
is 2000. `mtkx_coordinator_poll_safepoint(c, id)` marks the mutator
`AT_SAFEPOINT` and waits up to `MTKX_SAFEPOINT_POLL_TIMEOUT_MS` (5000)
for the generation to change. `mtkx_coordinator_release_mutators`
advances the generation and sets every record back to `RUNNING`.

`mtkx_mutator_context` and `mtkx_collector_context` are thin handles over
these calls. A stop request also changes the generation, so a poll that
was already waiting can return because of the request rather than the
release. Build an explicit parking protocol on top of these primitives.

### 11.4 A deterministic application-managed park

The mutator publishes `AT_SAFEPOINT` and stays parked until the
collector's release sets it back to `RUNNING`. Both sides read that state
through the coordinator, so the handshake is properly synchronised.

```c
#include <memtkx/memtkx.h>

#include <assert.h>
#include <pthread.h>
#include <sched.h>

static mtkx_status provide_roots(mtkx_address_vec *roots, void *ctx) {
  (void)ctx;
  return mtkx_address_vec_push(roots, 123);
}

static void *mutator(void *arg) {
  mtkx_coordinator *c = (mtkx_coordinator *)arg;
  /* managed writes finish before publishing the safepoint */
  mtkx_coordinator_update_status(c, 1, MTKX_MUTATOR_AT_SAFEPOINT);
  while (mtkx_coordinator_status(c, 1) == MTKX_MUTATOR_AT_SAFEPOINT) {
    sched_yield(); /* parked: no managed memory is touched here */
  }
  return NULL;
}

int main(void) {
  mtkx_coordinator *c = mtkx_coordinator_create(1);
  mtkx_thread_record record;
  pthread_t thread;
  mtkx_collector_context collector;
  mtkx_address_vec roots = MTKX_ADDRESS_VEC_INIT;
  mtkx_gc_result stopped;
  mtkx_status status;
  bool removed;

  if (c == NULL) {
    return 1;
  }
  mtkx_thread_record_init(&record, 1, "application-mutator");
  record.root_provider = provide_roots;
  if (mtkx_coordinator_register_mutator(c, &record) != MTKX_OK) {
    return 1;
  }
  pthread_create(&thread, NULL, mutator, c);

  collector = mtkx_collector_context_make(c);
  stopped = mtkx_collector_context_stop_the_world(&collector,
                                                  MTKX_DEFAULT_STOP_THE_WORLD_TIMEOUT_MS);
  assert(stopped.ok);
  status = mtkx_collector_context_roots(&collector, &roots);
  assert(status == MTKX_OK && roots.size == 1 && roots.data[0] == 123);
  /* collect with the gathered roots here, while the mutator is parked */

  mtkx_collector_context_release_mutators(&collector);
  pthread_join(thread, NULL);
  removed = mtkx_coordinator_unregister_mutator(c, 1);
  assert(removed);
  mtkx_address_vec_destroy(&roots);
  mtkx_coordinator_destroy(c);
  return 0;
}
```

(The example uses POSIX threads. The library itself also supports Win32.)

### 11.5 Root callback execution

`mtkx_coordinator_collect_roots` runs every stack scanner and root
provider while holding the coordinator mutex, appending to one vector. A
callback must not call back into the coordinator. A non-`MTKX_OK` status
from a callback stops root collection and is returned as is. Collecting
roots does not check that mutators are stopped; call it inside your own
quiescent interval.

### 11.6 Thread-safety boundaries

Only the coordinator is synchronised. Allocators, spaces, plans, queues,
card tables, SATB queues, parsers, and the panic and allocator hook
settings are not. Use per-thread state or your own lock, and serialise
collection against everything that mutates plan metadata.

**Learning task:** add a second parked thread with `expected_mutators`
set to two, give each thread its own root provider, and check the
combined roots without assuming any callback order.

## Chapter 12 — The DSL toolkit in C

The DSL headers (`<memtkx/dsl.h>`) work independently of the memory
toolkit. Each C++ feature becomes a family of C types and functions:

| C++ feature | C header | C mechanism |
| --- | --- | --- |
| `DSL<Derived, Features...>` | `dsl/features.h` | `mtkx_feature` bit flags, `mtkx_has_feature` |
| `Result<T,E>`, `Maybe<T>` | `dsl/result.h` | `MTKX_DEFINE_RESULT`, `MTKX_DEFINE_MAYBE` |
| `pipe`, `operator|` | `dsl/pipeline.h` | `mtkx_pipe_stage[]`, `mtkx_pipe_run`, `MTKX_PIPE` |
| `predicate`, `&`, `|`, `!` | `dsl/pipeline.h` | `mtkx_predicate` trees |
| `match`, `when`, `otherwise`, `pattern` | `dsl/match.h` | `mtkx_match_clause[]`, `mtkx_pattern_matches` |
| `ASTNode`, `leaf`, `node`, `rewrite_set` | `dsl/ast.h` | `mtkx_ast_node`, `mtkx_rewrite_set` |
| `ExprTemplates` | `dsl/expr.h` | `mtkx_expr` trees, scalar and element-wise |
| `literal_set`, `lit` | `dsl/literals.h` | `mtkx_literal_set` |
| `memoize`, `Lazy` | `dsl/memo.h` | `mtkx_memo`, `mtkx_lazy` |
| `Parser<T>` and combinators | `dsl/parser.h` | ref-counted `mtkx_parser`, dynamic `mtkx_parse_value` |
| PEG definitions, matcher, combinators | `dsl/peg.h` | `mtkx_peg_*` |
| `Task`, `TaskChain`, `run` | `dsl/task.h` | `mtkx_task_chain`, `mtkx_task_run` |

### 12.1 Generic results and optionals

`MTKX_DEFINE_RESULT(Name, T, E);` and `MTKX_DEFINE_MAYBE(Name, T);` create
a struct type and `static inline` helpers (§3.5), which cost nothing until
you use them. Unlike the C++ variant-based version, `Result` works when
`T` and `E` are the same type. To map into a *different* result or maybe
type, use `MTKX_RESULT_MAP(Out, r, fn, ctx)`, `MTKX_RESULT_MAP_ERR`,
`MTKX_RESULT_AND_THEN`, `MTKX_MAYBE_MAP`, and `MTKX_MAYBE_FLAT_MAP`. These
macros evaluate `r` more than once, so pass a variable. MemTKX predefines
`mtkx_maybe_int`, `_long`, `_size`, `_uintptr`, `_double`, and `_char`.

```c
#include <memtkx/dsl.h>

#include <assert.h>

MTKX_DEFINE_RESULT(parse_result, int, const char *);
MTKX_DEFINE_RESULT(ratio_result, double, const char *);

static parse_result positive(int v, void *ctx) {
  (void)ctx;
  return v > 0 ? parse_result_from_ok(v) : parse_result_from_err("not positive");
}
static double half(int v, void *ctx) {
  (void)ctx;
  return v / 2.0;
}
static int plus_one(int v, void *ctx) {
  (void)ctx;
  return v + 1;
}

int main(void) {
  parse_result r = parse_result_and_then(parse_result_from_ok(9), positive, NULL);
  ratio_result h = MTKX_RESULT_MAP(ratio_result, r, half, NULL);
  assert(parse_result_unwrap(r) == 9 && ratio_result_unwrap(h) == 4.5);
  assert(parse_result_is_err(parse_result_and_then(parse_result_from_ok(-1), positive, NULL)));
  assert(mtkx_maybe_int_or_else(mtkx_maybe_int_map(mtkx_maybe_int_some(5), plus_one, NULL), 0) == 6);
  return 0;
}
```

### 12.2 Pipelines and predicates

A pipeline stage is `bool fn(void *value, void *ctx)`. It transforms the
value in place and returns `false` to stop the rest of the pipeline (the
C++ `std::nullopt`). `mtkx_pipe_run(&value, stages, n)` runs the stages,
and in C the `MTKX_PIPE(&value, stage, ...)` macro does the same with a
compound literal. Predicates are small trees:
`mtkx_predicate_make(fn, ctx)`, `mtkx_predicate_and(&a, &b)`, `_or`, and
`_not`. `mtkx_predicate_test(&p, &value)` evaluates one with
short-circuiting. Composites refer to their operands by pointer and never
allocate.

```c
#include <memtkx/dsl.h>

#include <assert.h>

static bool times_two(void *v, void *ctx) {
  (void)ctx;
  *(int *)v *= 2;
  return true;
}
static bool plus_one(void *v, void *ctx) {
  (void)ctx;
  *(int *)v += 1;
  return true;
}
static bool positive(const void *v, void *ctx) {
  (void)ctx;
  return *(const int *)v > 0;
}
static bool even(const void *v, void *ctx) {
  (void)ctx;
  return *(const int *)v % 2 == 0;
}

int main(void) {
  int value = 10;
  mtkx_predicate pos = mtkx_predicate_make(positive, NULL);
  mtkx_predicate ev = mtkx_predicate_make(even, NULL);
  mtkx_predicate accepted = mtkx_predicate_and(&pos, &ev);
  int four = 4, minus_two = -2, three = 3;
  bool completed = MTKX_PIPE(&value, mtkx_pipe(times_two, NULL), mtkx_pipe(plus_one, NULL));

  assert(completed && value == 21);
  assert(mtkx_predicate_test(&accepted, &four));
  assert(!mtkx_predicate_test(&accepted, &minus_two) && !mtkx_predicate_test(&accepted, &three));
  return 0;
}
```

### 12.3 Dispatch tables

A table is an array of clauses: `mtkx_when(value, handler, ctx)` for
integer keys, `mtkx_when_pattern("[0-9]+", handler, ctx)` for string keys,
and `mtkx_otherwise(handler, ctx)`. `mtkx_match_int` and
`mtkx_match_string` run the first matching clause, or else the first
`otherwise`. They return `false` when nothing ran; the C++ table threw in
that case. A handler receives the key, your extra arguments, and a result
slot to fill.

```c
#include <memtkx/dsl.h>

#include <assert.h>

enum state { UNMARKED, MARKED, OTHER };

static void to_marked(mtkx_match_key key, void *args, void *out, void *ctx) {
  (void)key, (void)args, (void)ctx;
  *(enum state *)out = MARKED;
}
static void keep(mtkx_match_key key, void *args, void *out, void *ctx) {
  (void)args, (void)ctx;
  *(enum state *)out = (enum state)key.value;
}

int main(void) {
  mtkx_match_clause clauses[3];
  mtkx_match_table transition;
  enum state next = UNMARKED;
  bool fired;
  clauses[0] = mtkx_when(UNMARKED, to_marked, NULL);
  clauses[1] = mtkx_when(MARKED, to_marked, NULL);
  clauses[2] = mtkx_otherwise(keep, NULL);
  transition = mtkx_match_table_make(clauses, 3);

  fired = mtkx_match_int(&transition, UNMARKED, NULL, &next);
  assert(fired && next == MARKED);
  fired = mtkx_match_int(&transition, OTHER, NULL, &next);
  assert(fired && next == OTHER);
  assert(mtkx_pattern_matches("[a-z][a-z0-9_]*", "node_7"));
  return 0;
}
```

The pattern language is a small subset: literals, `.`, `[abc]`, `[a-z]`,
`*`, and `+`. A leading `^` is ignored, and matches are always anchored at
both ends.

### 12.4 AST construction and rewrite rules

Nodes live on the heap. `mtkx_ast_leaf(tag, value)`,
`mtkx_ast_leaf_int`, `_uint`, `_double` (formatted with `%g`), and
`_fmt` create leaves. `mtkx_ast_node_of(tag, count, ...)` and
`mtkx_ast_node_new` create inner nodes and take ownership of their
children. A `NULL` child (from a failed allocation) makes the call free
everything and return `NULL`, so a whole nested expression needs one
check. `mtkx_ast_dump` returns the S-expression as a string you release
with `mtkx_free`. `mtkx_ast_free` releases a tree.

A rewrite rule pairs a predicate with a transformer that returns a new,
owned replacement tree; use `mtkx_ast_clone` on parts of the input.
`mtkx_rewrite_apply(&set, root)` rewrites a copy bottom-up until no rule
fires or `max_iterations` passes have run (default 100). The input is
left unchanged.

```c
#include <memtkx/dsl.h>

#include <assert.h>
#include <string.h>

static bool is_identity(const mtkx_ast_node *n, void *ctx) {
  (void)ctx;
  return mtkx_ast_tag_is(n, "identity") && mtkx_ast_size(n) == 1;
}
static mtkx_ast_node *unwrap(const mtkx_ast_node *n, void *ctx) {
  (void)ctx;
  return mtkx_ast_clone(mtkx_ast_child(n, 0));
}

int main(void) {
  mtkx_ast_node *tree = mtkx_ast_node_of("identity", 1, mtkx_ast_leaf_int("number", 42));
  mtkx_rewrite_rule rules[1];
  mtkx_rewrite_set set;
  mtkx_ast_node *simplified;
  char *text;

  rules[0] = mtkx_rewrite_rule_make("remove-identity", is_identity, unwrap, NULL);
  set = mtkx_rewrite_set_make(rules, 1);
  simplified = mtkx_rewrite_apply(&set, tree);
  text = mtkx_ast_dump(simplified);
  assert(strcmp(text, "(number 42)") == 0);

  mtkx_free(text);
  mtkx_ast_free(simplified);
  mtkx_ast_free(tree);
  return 0;
}
```

### 12.5 Expression trees

`mtkx_expr` nodes describe arithmetic without performing it. Terminals
are `mtkx_expr_const`, `mtkx_expr_terminal(value_fn, self)` (the C++
`expr_value()`), and `mtkx_expr_array(data, n)`. Operators are `_add`,
`_sub`, `_mul`, `_div`, and `_neg`. Nodes point at their operands, so
building a tree never allocates. `mtkx_expr_eval` evaluates a scalar tree.
`mtkx_expr_eval_into(&e, out, n)` evaluates element-wise in one pass,
with constants and callbacks broadcast across the elements.

### 12.6 Literal tables, memoisation, and lazy values

`mtkx_literal_set_parse(&set, "3.5_km", &out)` tries each suffix in order
and converts the number like `strtold`. `mtkx_memo` caches fixed-size
values by fixed-size keys, compared byte by byte, so zero any padding in
struct keys. `mtkx_lazy` computes a value into your storage the first
time you call `mtkx_lazy_get`. None of these is synchronised.

```c
#include <memtkx/dsl.h>

#include <assert.h>

static long double km(long double v, void *ctx) {
  (void)ctx;
  return v * 1000.0L;
}
static void square(const void *key, void *out, void *ctx) {
  ++*(int *)ctx;
  *(int *)out = *(const int *)key * *(const int *)key;
}
static void answer(void *out, void *ctx) {
  (void)ctx;
  *(int *)out = 42;
}

int main(void) {
  mtkx_literal_entry units[1];
  mtkx_literal_set set;
  long double distance = 0.0L;
  mtkx_status status;
  mtkx_memo memo;
  int computations = 0, seven = 7, result = 0, storage = 0;
  mtkx_lazy deferred;

  units[0] = mtkx_lit("_km", km, NULL);
  set = mtkx_literal_set_make(units, 1);
  status = mtkx_literal_set_parse(&set, "3.5_km", &distance);
  assert(status == MTKX_OK && distance == 3500.0L);

  mtkx_memo_init(&memo, sizeof(int), sizeof(int), square, &computations);
  mtkx_memo_call(&memo, &seven, &result);
  mtkx_memo_call(&memo, &seven, &result);
  assert(result == 49 && computations == 1);
  mtkx_memo_destroy(&memo);

  mtkx_lazy_init(&deferred, &storage, answer, NULL);
  assert(!mtkx_lazy_is_computed(&deferred));
  mtkx_lazy_get(&deferred);
  assert(storage == 42 && mtkx_lazy_is_computed(&deferred));
  return 0;
}
```

### 12.7 Parser combinators

Without `Parser<T>`, C parsers produce dynamically typed values
(`mtkx_parse_value`) whose kinds mirror the C++ types:

| Combinator | C++ value | C kind |
| --- | --- | --- |
| `mtkx_parser_ch(c)`, `mtkx_parser_satisfy(pred, ctx, label)` | `char` | `MTKX_PV_CHAR` |
| `mtkx_parser_seq(a, b)` (C++ `a & b`) | `std::pair` | `MTKX_PV_PAIR` |
| `mtkx_parser_alt(a, b)` (C++ `a | b`) | `T` | whichever ran |
| `mtkx_parser_many(p)` (C++ `*p`) | `std::vector` | `MTKX_PV_LIST` |
| `mtkx_parser_optional(p)` | `std::optional` | `MTKX_PV_OPTIONAL` |
| `mtkx_parser_try(p)`, `mtkx_parser_labeled(p, label)` | `T` | unchanged |
| `mtkx_parser_new(fn, ctx, ctx_free)` | any | whatever `fn` returns, including `MTKX_PV_USER` |

Parsers are reference counted, and **every combinator consumes the
references you pass in**, so expressions nest. Use
`mtkx_parser_retain(p)` to use one parser in two places, and release your
own references with `mtkx_parser_release`. A combinator given `NULL`
releases its other operands and returns `NULL`.

`mtkx_run_parser(p, text, len)` requires the whole input to be consumed;
leftover input is a committed failure expecting `"<end-of-input>"`. A
result holds either an owned value or an `mtkx_parse_error` (position,
kind, expected labels). Release it with `mtkx_parse_result_free`.

```c
#include <memtkx/dsl.h>

#include <assert.h>

int main(void) {
  mtkx_parser *pair = mtkx_parser_seq(mtkx_parser_ch('a'), mtkx_parser_ch('b'));
  mtkx_parser *alternative;
  mtkx_parse_result good, trailing, other;

  good = mtkx_run_parser(pair, "ab", 2);
  assert(good.ok && good.value->kind == MTKX_PV_PAIR);
  assert(good.value->as.pair.first->as.ch == 'a' && good.value->as.pair.second->as.ch == 'b');
  mtkx_parse_result_free(&good);

  trailing = mtkx_run_parser(pair, "abc", 3);
  assert(!trailing.ok && trailing.error.pos == 2);
  mtkx_parse_result_free(&trailing);

  alternative = mtkx_parser_alt(mtkx_parser_try(mtkx_parser_retain(pair)),
                                mtkx_parser_seq(mtkx_parser_ch('a'), mtkx_parser_ch('c')));
  other = mtkx_run_parser(alternative, "ac", 2);
  assert(other.ok);
  mtkx_parse_result_free(&other);

  mtkx_parser_release(alternative);
  mtkx_parser_release(pair);
  return 0;
}
```

A sequence that fails after consuming input is a committed failure.
Ordered choice skips its second branch after a committed failure unless
the first branch is wrapped with `mtkx_parser_try`. Unlike the C++
original, a repetition whose operand succeeds without consuming input
stops instead of looping forever. `mtkx_parse_with_action` calls an action
with the matched span; `mtkx_production_make` and `mtkx_production_find`
key actions by id; `mtkx_load_parse_input_file` reads a whole file.

### 12.8 PEG grammars

A definition is an ordered rule set. `mtkx_peg_definition_add_rule(d,
pattern, action, ctx, &rule)` compiles the pattern; a malformed pattern
returns `MTKX_ERR_INVALID_ARGUMENT`, and `mtkx_peg_pattern_error` explains
why. After creation you can still change a rule's channel (with
`mtkx_peg_rule_set_channel`) and action, and its `must_fail`,
`fire_on_neg`, and `if_succeed` fields. `mtkx_peg_definition_derive`
creates a child definition that sees its parent's rules first.

`mtkx_peg_definition_parse` tokenizes by ordered choice, runs actions as
matches occur, and skips input on the `@IGNORE` channel. A rule that
matches empty input fails the parse instead of looping. The result
carries the offset, line, column, and a message.

```c
#include <memtkx/dsl.h>

#include <assert.h>

static void count(mtkx_peg_match *match, void *ctx) {
  (void)match;
  ++*(int *)ctx;
}

int main(void) {
  int idents = 0;
  mtkx_peg_definition *peg = mtkx_peg_definition_create();
  mtkx_peg_rule *space = NULL;
  mtkx_peg_parse_result r;

  mtkx_peg_definition_add_rule(peg, "[a-z][a-z0-9]*", count, &idents, NULL);
  mtkx_peg_definition_add_rule(peg, "[0-9]+", NULL, NULL, NULL);
  mtkx_peg_definition_add_rule(peg, "[ ]+", NULL, NULL, &space);
  mtkx_peg_rule_set_channel(space, MTKX_PEG_IGNORE_CHANNEL);

  r = mtkx_peg_definition_parse(peg, "ab 12 c3", 8);
  assert(r.success && idents == 2);
  r = mtkx_peg_definition_parse(peg, "ab #", 4);
  assert(!r.success && r.offset == 3 && r.column == 4);
  mtkx_peg_definition_destroy(peg);
  return 0;
}
```

`mtkx_peg_matcher` is the streaming counterpart of `matcher << arm`. Feed
it arms with `mtkx_peg_matcher_feed`:

* `mtkx_peg_rule_arm(rule, fn, ctx)` matches one rule.
* `mtkx_peg_wildcard_arm(fn, ctx)` fires when nothing else matched and
  consumes one character.
* `mtkx_peg_channel_arm(channel, fn, ctx)` matches any rule on that
  channel. (The C++ version had this field but no way to set it.)

`_only`, `_include`, and `_exclude` filter rule arms by channel. The PEG
combinators `mtkx_peg_seq`, `_choice`, `_opt`, `_many`, `_many1`, `_and`,
`_not`, `_rule_parser`, `_channel`, and `_as_span` build `mtkx_parser`
values whose results are mostly `MTKX_PV_MATCH` spans.

### 12.9 Feature flags

C has no mixins. When a component should advertise which DSL families it
supports, store an `unsigned` mask of `mtkx_feature` bits and test it with
`mtkx_has_feature`. `mtkx_feature_name` returns the C++ tag name for a
bit.

### 12.10 Task chains

`mtkx_task_chain` runs named tasks. Each returns an owned
`mtkx_task_result`, built with `mtkx_task_ok(text)` or
`mtkx_task_err(text)`. `mtkx_task_run(&state, &chain)` stores each result
under its task's name, replacing any earlier one. It stops after an error
under `MTKX_TASK_STOP_ON_ERROR`, or once a task calls `mtkx_task_suspend`.
`mtkx_task_upon` runs a task only when an event function returns true.

## Chapter 13 — Plugins and custom extensions

### 13.1 What a plugin means in MemTKX

A plugin is an application-defined component that fits an existing
extension point. You compile it into your application and select it
explicitly; there is no loader, manifest, or registry.

| Extension point | Your responsibility |
| --- | --- |
| `mtkx_space` + `mtkx_space_ops` | Allocation, ownership, metrics |
| `mtkx_plan` + `mtkx_plan_ops` | Plan identity, allocation, collection |
| `mtkx_trace` | Object-layout scanning |
| Root and stack callbacks | Published root discovery |
| `mtkx_phase` | One collection phase |
| `mtkx_barrier_custom` stages | Write-recording or filtering policy |
| `mtkx_set_allocator`, `mtkx_set_panic_handler` | Process-wide policy hooks |

### 13.2 A quota-controlled space plugin

Embed `mtkx_space` as the **first member** of your struct, so that a
`mtkx_space *` and a pointer to your struct convert to each other. Then
supply a vtable. `mtkx_space_allocate` validates the alignment and calls
your `allocate_impl`.

```c
#include <memtkx/memtkx.h>

#include <assert.h>

typedef struct quota_space {
  mtkx_space base; /* first member */
  mtkx_bump_allocator allocator;
  size_t quota;
  size_t charged;
} quota_space;

static mtkx_alloc_result quota_allocate(mtkx_space *space, size_t bytes, size_t alignment) {
  quota_space *q = (quota_space *)space;
  mtkx_alloc_result result;
  if (bytes > q->quota - q->charged) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
  }
  result = mtkx_bump_allocator_allocate(&q->allocator, bytes, alignment);
  if (result.ok) {
    q->charged += bytes;
    mtkx_space_record_allocation(space, bytes);
  }
  return result;
}

static const mtkx_space_ops quota_ops = {quota_allocate, NULL};

static double storage[4096 / sizeof(double)];

int main(void) {
  const mtkx_address start = mtkx_as_address(storage);
  quota_space space;
  mtkx_alloc_result fits, over_quota, misaligned;

  mtkx_bump_allocator_init(&space.allocator, start, start + sizeof storage);
  space.quota = 128;
  space.charged = 0;
  if (mtkx_space_init(&space.base, &quota_ops, "quota", MTKX_SPACE_BUMP_POINTER,
                      mtkx_range(start, start + sizeof storage), MTKX_MAX_ALIGN) != MTKX_OK) {
    return 1;
  }

  fits = mtkx_space_allocate(&space.base, 96, 16);
  over_quota = mtkx_space_allocate(&space.base, 64, 16);
  misaligned = mtkx_space_allocate(&space.base, 1, 3);
  assert(fits.ok && !over_quota.ok && space.charged == 96);
  assert(mtkx_space_metrics_of(&space.base)->total_allocations == 1);
  assert(misaligned.error == MTKX_ALLOC_MISALIGNED);

  /* this plugin's reset policy also clears its metrics */
  mtkx_bump_allocator_reset(&space.allocator);
  space.charged = 0;
  mtkx_space_metrics_reset(&space.base.metrics);
  assert(mtkx_space_metrics_of(&space.base)->live_bytes == 0);
  mtkx_space_destroy(&space.base);
  return 0;
}
```

`charged <= quota` holds because only successful allocations increase
`charged`, so the subtraction cannot wrap. `mtkx_space_destroy` calls
`ops->destroy` (if any) and frees the copied name.

### 13.3 A collection-observer plan plugin

A plan plugin embeds `mtkx_plan` first and supplies `kind`, `name`,
`allocate`, `collect_impl`, and optionally `destroy`. This adapter wraps
a mark-sweep plan and notifies an observer after each successful
collection.

```c
#include <memtkx/memtkx.h>

#include <assert.h>
#include <string.h>

typedef struct observed_plan {
  mtkx_plan base; /* first member */
  mtkx_mark_sweep_plan *inner;
  size_t observations;
} observed_plan;

static mtkx_plan_kind observed_kind(const mtkx_plan *p) {
  (void)p;
  return MTKX_PLAN_MARK_SWEEP;
}
static const char *observed_name(const mtkx_plan *p) {
  (void)p;
  return "ObservedMarkSweep";
}
static mtkx_alloc_result observed_allocate(mtkx_plan *p, size_t bytes, size_t alignment) {
  return mtkx_mark_sweep_plan_allocate(((observed_plan *)p)->inner, bytes, alignment);
}
static mtkx_plan_stats_result observed_collect(mtkx_plan *p, const mtkx_address *roots,
                                               size_t count, mtkx_trace trace) {
  observed_plan *o = (observed_plan *)p;
  mtkx_plan_stats_result r = mtkx_mark_sweep_plan_collect(o->inner, roots, count, trace);
  if (r.ok) {
    ++o->observations;
    assert(r.value.collection_count == o->observations);
  }
  return r;
}
static void observed_destroy(mtkx_plan *p) {
  mtkx_mark_sweep_plan_destroy(((observed_plan *)p)->inner);
}

static const mtkx_plan_ops observed_ops = {observed_kind, observed_name, observed_allocate,
                                           observed_collect, observed_destroy};

int main(void) {
  observed_plan heap;
  mtkx_alloc_result allocated;
  mtkx_plan_stats_result collected;
  heap.base.ops = &observed_ops;
  heap.inner = mtkx_mark_sweep_plan_create(4096);
  heap.observations = 0;
  if (heap.inner == NULL) {
    return 1;
  }

  allocated = mtkx_plan_allocate(&heap.base, 64, 16);
  assert(allocated.ok);
  collected = mtkx_plan_collect(&heap.base, &allocated.value, 1, mtkx_trace_none());
  assert(collected.ok && heap.observations == 1);
  assert(mtkx_plan_get_kind(&heap.base) == MTKX_PLAN_MARK_SWEEP);
  assert(strcmp(mtkx_plan_name(&heap.base), "ObservedMarkSweep") == 0);
  mtkx_plan_destroy(&heap.base); /* releases the inner plan */
  return 0;
}
```

`mtkx_plan_kind` is a closed set of the four built-in kinds. An adapter
reports the kind it wraps. A genuinely new algorithm should identify
itself through `name` rather than invent an enum value.

### 13.4 Callback plugins

Trace callbacks and root providers are lightweight plugins. The `ctx`
pointer carries their state, and that state must outlive every call.
Root providers run under the coordinator mutex; trace callbacks run
inside a collection. Document which thread may call each plugin and
whether its state is read-only.

### 13.5 Barrier, phase, and policy plugins

A barrier plugin can be an `mtkx_barrier_custom` stage. It can also
combine `mtkx_satb_barrier_write` and `mtkx_card_table_barrier_apply`
behind one store function; capture the old value before overwriting. A
phase plugin is an `mtkx_phase`. Process-wide policy goes in the
allocator and panic hooks (§3.7).

### 13.6 Packaging a plugin

Put plugin headers in your own include tree and prefix, not `mtkx_`.

```cmake
find_package(MemTKX 2 CONFIG REQUIRED)
add_library(ApplicationMemoryPlugins STATIC quota_space.c observed_plan.c)
target_include_directories(ApplicationMemoryPlugins PUBLIC include)
target_link_libraries(ApplicationMemoryPlugins PUBLIC MemTKX::MemTKX)
```

If you export the plugin as an installed package, call
`find_dependency(MemTKX 2 CONFIG)` in its config before loading its
targets.

### 13.7 Plugin contract template

Document these facts for every extension:

- **Storage:** owned or borrowed, and which owner must stay alive.
- **Allocation:** size and alignment requirements, and failure results.
- **Reclamation:** individual free, whole-region reset, or collection.
- **References:** root representation, trace format, relocation behavior.
- **Execution:** calling thread, synchronisation, and callback lifetime.
- **Metrics:** requested bytes versus consumed capacity, and reset rules.
- **Composition:** required first member, vtable, delegates, and phases.

## Chapter 14 — The C++ wrapper

### 14.1 What it is

`<memtkx/MemTKX.hpp>` and `<memtkx/MemTKXDSL.hpp>` form a header-only
C++17 layer over the C library. It keeps the original `MemTKX` and
`MemTKX::dsl` APIs, and the original nine C++ tests compile against it.
Link `MemTKX::MemTKXCpp`.

Each wrapper class owns or views a C object:

| C++ | C | Interop accessor |
| --- | --- | --- |
| `BumpPointerAllocator` (value type) | `mtkx_bump_allocator` | `c_allocator()` |
| `BumpSpace`, `FreeListSpace`, `ImmixSpace`, `LargeObjectSpace` | the C spaces | `c_space()` |
| `FreeListAllocator`, `SegregatedFreeListAllocator` (move-only) | the C allocators | `c_allocator()` |
| `MarkSweepPlan` … `GenerationalPlan` (move-only) | the opaque C plans | `c_plan()` |
| `CardTable`, `SatbQueue`, `WorkQueue` | the C structs | `c_table()`, `c_queue()` |
| `CollectorCoordinator` | `mtkx_coordinator` | `c_coordinator()` |
| `Slot`, `ReadSlot` | **are** `mtkx_slot`, `mtkx_read_slot` | — |
| `dsl::ASTNode` | `mtkx_ast_node` | `c_node()`, `adopt()`, `release()` |
| `dsl::Parser<T>` | `mtkx_parser` | `c_parser()`, `retain_c()` |
| `dsl::PEGDefinition`, `PEGRule`, `PEGMatcher` | the C PEG objects | `c_definition()`, `c_rule()`, `c_matcher()` |

Compile-time idioms are kept as C++ templates: `DSL<Derived,
Features...>`, `Predicate`, `pipe`, `match`/`when`/`otherwise`, expression
templates, `Result`, `Maybe`, `Lazy`, and `memoize`. Everything with
runtime behaviour runs in C: pattern matching, ASTs and rewriting, literal
parsing, parser combinators, PEG, task chains, and all memory management.

### 14.2 The original example, unchanged

```cpp
#include <memtkx/MemTKX.hpp>

#include <iostream>
#include <new>
#include <vector>

struct Node {
  MemTKX::Address next{0};
  int value{0};
};

int main() {
  using namespace MemTKX::literals;
  MemTKX::MarkSweepPlan heap(64_KB);

  auto allocation = heap.allocate(sizeof(Node), alignof(Node));
  if (allocation.is_err()) {
    return 1;
  }
  const auto root = allocation.unwrap();
  auto* node = new (MemTKX::from_address<void>(root)) Node{};
  node->value = 42;

  MemTKX::TraceFn trace = [](MemTKX::Address address) {
    const auto* current = MemTKX::from_address<const Node>(address);
    std::vector<MemTKX::Address> children;
    if (current->next != 0) {
      children.push_back(current->next);
    }
    return children;
  };

  auto collected = heap.collect({root}, trace);
  if (collected.is_err()) {
    return 2;
  }
  const auto stats = collected.unwrap();
  std::cout << "value=" << node->value << ", live_objects=" << stats.live_objects
            << ", collections=" << stats.collection_count << '\n';
  node->~Node();
  return heap.collect({}, trace).is_ok() ? 0 : 3;
}
```

### 14.3 C++ plugins usable from C

`SpaceBase<Derived>` and `PlanBase<Derived>` work as before: pass a
`SpaceDescriptor`, implement `allocate_impl`, use the protected
`record_*` helpers and the `metrics_` member, or implement `kind_value`,
`name_value`, and `collect_impl`. Each plugin is also backed by a C
vtable, so C code can drive it:

```cpp
#include <memtkx/MemTKX.hpp>

#include <cassert>
#include <cstring>

class CountingSpace final : public MemTKX::SpaceBase<CountingSpace> {
 public:
  CountingSpace(MemTKX::Address start, MemTKX::Address end)
      : SpaceBase(MemTKX::SpaceDescriptor{"counting", MemTKX::SpaceKind::BumpPointer,
                                          {start, end}, alignof(std::max_align_t)}),
        bump_(start, end) {}

  MemTKX::AllocResult<MemTKX::Address> allocate_impl(std::size_t bytes, std::size_t alignment) {
    auto r = bump_.allocate(bytes, alignment);
    if (r.is_ok()) {
      record_allocation(bytes);
    }
    return r;
  }

 private:
  MemTKX::BumpPointerAllocator bump_;
};

int main() {
  alignas(std::max_align_t) static unsigned char storage[1024];
  CountingSpace space(MemTKX::as_address(storage), MemTKX::as_address(storage) + sizeof storage);
  mtkx_space* c = space.c_space();                 // hand this to C code
  const mtkx_alloc_result r = mtkx_space_allocate(c, 64, 16);  // calls allocate_impl
  assert(r.ok);
  assert(std::strcmp(mtkx_space_name(c), "counting") == 0);
  assert(space.metrics().total_allocations == 1);  // one shared set of metrics
}
```

`c_plan()` does the same for plans: a C trace becomes a `TraceFn`, and a
C trace that reports failure turns into `MTKX_GC_INVALID_TRACE`. The
pointer is invalidated when the plan object is moved.

### 14.4 Exceptions and C frames

C++ exceptions never unwind through C. Every trampoline catches the
exception, stores it in a thread-local slot, and reports failure to C,
and the wrapper call that entered C rethrows it. Callbacks that can
throw include trace functions, root providers, phases, parser functions,
rewrite predicates, PEG actions, literal handlers, task functions, and
plugin methods. A throwing phase stops its pipeline, and a throwing trace
aborts a collection before anything is swept.

### 14.5 Standard requirements

The wrapper needs C++17. The string-literal template forms need C++20:
`leaf<"tag">`, `node<"tag">`, `rule<"name">`, `pattern<"...">`,
`lit<"_km">`, `add_rule<"...">`, and `production<"id">`, as do the
`HasFeature`/`Pipeable`/`ExprLike`/`HasLiterals`/`Rewritable` concepts.
Runtime-string versions work in C++17: `leaf("tag", v)`,
`node("tag", ...)`, `rule("name", p, t)`, `dsl::Pattern("...")`,
`lit("_km", f)`, `add_rule("pattern", action)`, and
`production("id", f)`.

### 14.6 Differences from the header-only version

* Spaces and plans cannot be copied; plans and the free-list allocators
  can be moved. `metrics()` returns a snapshot by value, and the built-in
  spaces' `allocator()` returns a snapshot (bump) or a read-only view
  (free list).
* `ImmixSpace::blocks()` returns copies of the blocks.
* `dsl::ASTNode::children()` and `child(i)` are read-only views of the C
  tree. Mutate with `append_child` / `take_child`; copying a view
  produces an independent deep copy.
* `PEGRule` cannot be copied. Its public fields are pushed to C whenever
  the definition parses, a matcher is fed, or `peg_rule()` is called.
  Parsers created earlier with `peg_rule()` see field changes only after
  the next push.
* `dsl::Result` also offers `unwrap_err()`, and `Result<T, T>` works.
* `SlotBuffer` (an RAII C slot buffer) and a `run_write_barrier` overload
  that records entirely in C are new.
* New accessors: `GenerationalPlan::dirty_cards()` and
  `PEGMatcher::on_channel()`.

## Chapter 15 — Testing, performance, and maintenance

### 15.1 The verification suite

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

| Test | Main subject |
| --- | --- |
| `c.test_units` | Units, ranges, alignment, runtime unit parsing |
| `c.test_allocator` | Bump (and fast path), free list, segregated, Immix, large objects, metrics |
| `c.test_barrier` | Stage chains, card boundaries, SATB, card barrier |
| `c.test_immix` | Rounded capacity and line marking |
| `c.test_large_object` | Large-space accounting |
| `c.test_scheduler_edges` | Timeouts, duplicates, phase error policy |
| `c.test_dispatcher` | Dispatch tables, patterns, predicates, pipelines, feature flags |
| `c.test_scheduler` | Ring-buffer queue, pipelines, cross-thread coordination |
| `c.test_plans` | Plan statistics, graphs, cycles, polymorphic plans, failing traces |
| `c.test_plugins` | Quota space and observed plan plugins |
| `c.test_dsl_values` | Result/Maybe macros, memo, lazy, literal sets, expressions |
| `c.test_dsl_ast` | AST construction and rewrite fixpoints |
| `c.test_dsl_parser` | Combinators, errors, user values, actions, file input |
| `c.test_dsl_peg` | Rules, channels, derivation, matcher, PEG combinators |
| `c.test_dsl_task` | Task chain policies, suspend, upon |
| `c.test_oom` | Fault injection: every allocation site fails once, cleanly |
| `cpp.test_*` (9) | The original C++ tests compiled against the wrapper |
| `cpp.test_interop` | C++ plugins through C, C objects in C++, exception transport |
| `cpp.test_dsl` | The DSL wrapper in C++20 |

`c.test_oom` installs allocator hooks that fail the Nth allocation, for
every N. That covers about 190 allocation sites, and after each run it
checks that nothing leaked. The suite also passes under
AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer, and
valgrind:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan && ctest --test-dir build-asan
```

### 15.2 Verify application semantics

For a real runtime, test a rooted chain, a rooted cycle, an unrooted
cycle, duplicate roots, an empty root set, and repeated collection after
field updates, and check object contents as well as counts. Run your own
workload under `test_oom`-style fault injection with
`mtkx_set_allocator`. Build an installed-package consumer as part of each
release.

### 15.3 Benchmark commands and interpretation

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DMemTKX_BUILD_TESTS=OFF -DMemTKX_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/benchmarks/bench_alloc
./build-bench/benchmarks/bench_barrier
```

`bench_alloc` runs 200,000 iterations. The bump loops reset on every
iteration: `bump-pointer` calls the exported function and
`bump-pointer-inline` uses `mtkx_bump_allocator_try_allocate`. The
free-list loop allocates and frees, and the Immix loop resets its blocks
every 256 iterations. `bench_barrier` runs 1,000,000 recording
iterations against a raw range filter, and keeps the raw hit count
observable so the compiler cannot delete that loop.

One reference run (GCC 13, `-O3`, x86-64 VM), compared with the original
header-only C++:

| Operation | C library | Original C++ |
| --- | --- | --- |
| bump, exported call | ~11 ns | — |
| bump, inline fast path | ~0.6 ns | ~0.5 ns (loop largely optimised away) |
| free list alloc+free | ~22 ns | ~103 ns (re-sorted on every operation) |
| Immix | ~16 ns | ~10 ns |
| barrier pipeline | ~19 ns | ~27 ns |

Use these only as a sanity check. For decisions, measure a representative
workload with the same compiler options for every variant.

### 15.4 Metrics that answer different questions

| Quantity | Source |
| --- | --- |
| Successful requested bytes | Allocator counters or space metrics |
| Cursor consumption, including padding | Bump/Immix `used_bytes` |
| Current free-list capacity | `free_bytes` and `free_cells` |
| Reachable object payload | `mtkx_plan_stats.live_bytes` |
| Occupied Immix line capacity | `marked_bytes` |
| Collection time | Your own monotonic clock |
| Bookkeeping memory | Count it with `mtkx_set_allocator` hooks |

### 15.5 Troubleshooting by symptom

| Symptom | What to inspect |
| --- | --- |
| Header not found | Include `<memtkx/memtkx.h>` and link `MemTKX::MemTKX` |
| Undefined `mtkx_*` symbols | Link `libmemtkx` (and `-pthread`) |
| `dllimport` link errors on Windows | Define `MTKX_SHARED` when using a shared build without CMake |
| `MTKX_ALLOC_MISALIGNED` | Pass a nonzero power-of-two alignment |
| `MTKX_ALLOC_ALLOCATION_FAILED` | A zero-byte request, or bookkeeping ran out of memory |
| `MTKX_GC_INVALID_TRACE` | Your trace callback returned `false` (perhaps a failed push) |
| `MTKX_GC_PLAN_UNAVAILABLE` | Promotion capacity, or plan bookkeeping out of memory |
| Allocation fails despite free bytes | Fragmentation, partition capacity, padding |
| Immix collection does not restore space | The built-in plan marks but does not sweep |
| Large-space deallocation does not restore space | Freeing one object only updates accounting; reset the whole space |
| Copying-plan references are stale | Roots and fields are not rewritten |
| Safepoint times out | Actual parking, the expected count, every record's status |
| Root callback deadlocks | It called back into the coordinator |
| Pipeline reports zero completed steps | Phases must call `mtkx_phase_state_note_completion` |
| Program aborts with "MemTKX panic" | An `*_unwrap` on the wrong branch; install a panic handler or check first |
| Parser memory grows | Release parsers and free every `mtkx_parse_result` |

### 15.6 C++ to C name map

| C++ | C |
| --- | --- |
| `Address`, `AddressRange` | `mtkx_address`, `mtkx_address_range` |
| `align_up`, `is_power_of_two`, `can_allocate` | `mtkx_align_up`, `mtkx_is_power_of_two`, `mtkx_can_allocate` |
| `64_KB`, `bytes_to_kib` | `MTKX_KB(64)`, `mtkx_bytes_to_kib` |
| `MemoryUnitsDSL::parse_literal` | `mtkx_units_parse_literal` |
| `AllocError::OutOfMemory`, `to_string` | `MTKX_ALLOC_OUT_OF_MEMORY`, `mtkx_alloc_error_string` |
| `AllocResult<Address>`, `GcResult<int>` | `mtkx_alloc_result`, `mtkx_gc_result` |
| `GcResult<PlanStats>` | `mtkx_plan_stats_result` |
| `BumpPointerAllocator`, `BumpSpace` | `mtkx_bump_allocator`, `mtkx_bump_space` |
| `FreeListAllocator`, `FreeListSpace` | `mtkx_free_list_allocator`, `mtkx_free_list_space` |
| `SegregatedFreeListAllocator` | `mtkx_segregated_allocator` |
| `ImmixBlock`, `ImmixSpace`, `kImmixBlockBytes` | `mtkx_immix_block`, `mtkx_immix_space`, `MTKX_IMMIX_BLOCK_BYTES` |
| `LargeObjectSpace` | `mtkx_los_space` |
| `SpaceBase<Derived>`, `SpaceMetrics` | `mtkx_space` + `mtkx_space_ops`, `mtkx_space_metrics` |
| `Slot`, `run_write_barrier`, `barrier_*` | `mtkx_slot`, `mtkx_run_write_barrier`, `mtkx_barrier_*` |
| `CardTable`, `CardTableBarrier` | `mtkx_card_table`, `mtkx_card_table_barrier` |
| `SatbQueue`, `SatbBarrier` | `mtkx_satb_queue`, `mtkx_satb_barrier` |
| `WorkPacket`, `WorkQueue` | `mtkx_work_packet`, `mtkx_work_queue` |
| `Phase`, `TaskPipeline`, `PhaseState`, `make_phase` | `mtkx_phase`, `mtkx_task_pipeline`, `mtkx_phase_state`, `mtkx_make_phase` |
| `CollectorCoordinator`, `ThreadRecord` | `mtkx_coordinator`, `mtkx_thread_record` |
| `MutatorContext`, `CollectorContext` | `mtkx_mutator_context`, `mtkx_collector_context` |
| `PlanBase<Derived>`, `PlanKind`, `PlanStats`, `TraceFn` | `mtkx_plan` + `mtkx_plan_ops`, `mtkx_plan_kind`, `mtkx_plan_stats`, `mtkx_trace` |
| `MarkSweepPlan` … `GenerationalPlan` | `mtkx_mark_sweep_plan` … `mtkx_generational_plan` |
| `dsl::*` | see the table at the start of chapter 12 |

### 15.7 Behavioural differences from 1.x, by design

* `can_allocate` no longer accepts an aligned start that is beyond the
  limit (the C++ subtraction wrapped around).
* Marking a zero-byte Immix extent marks nothing (the C++ version could
  mark every line of a block).
* A parser repetition that succeeds without consuming input stops instead
  of looping forever.
* A dispatch table with no matching clause returns `false` instead of
  throwing, and literal parsing returns a status instead of throwing.
* A barrier filter that rejects a slot passes "none" to later stages, so
  stages can be chained in any order.
* Bookkeeping memory failures are reported as values instead of
  `std::bad_alloc`; the C++ wrapper turns them back into `std::bad_alloc`.

### 15.8 Reading and extending the source

Start with `include/memtkx/core/`, then the allocator you use.
`space/space.h` defines the space contract and `plan/plan.h` the plan
contract. The implementations in `src/` are short. `src/internal/` holds
the private hash map, string buffer, and platform layer. Keep
application code out of the `mtkx_` namespace prefix. When you change
behavior, update the documentation and tests, and rebuild an installed
consumer.
