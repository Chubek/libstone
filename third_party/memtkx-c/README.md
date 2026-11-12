# MemTKX

**MemTKX is a C99 memory-management toolkit for building allocators,
garbage-collection experiments, and runtime integrations.** It provides
allocation spaces, four collection plans, write-barrier building blocks,
collection-phase pipelines, cooperative mutator coordination, and a DSL
toolkit (results, pipelines, dispatch tables, ASTs, parser combinators and
PEG grammars). It compiles as an ordinary static or shared C library with
CMake. A header-only C++17 wrapper keeps the original `MemTKX::` C++ API on
top of it.

Start with the working example below, then follow the
[15-chapter manual in GUIDE.md](GUIDE.md). It covers object tracing,
thread coordination, custom spaces and plans written in C, the C++ wrapper,
and reference tables.

## At a glance

| Item | Value |
| --- | --- |
| Library identity | `MemTKX` |
| Language | C99 (library); C++17 or later (optional wrapper) |
| Distribution | Compiled library (static by default, shared with `BUILD_SHARED_LIBS=ON`) |
| Main C include | `<memtkx/memtkx.h>` |
| DSL C include | `<memtkx/dsl.h>` (also included by `memtkx.h`) |
| C naming | `mtkx_` functions and types, `MTKX_` macros |
| C++ includes | `<memtkx/MemTKX.hpp>`, `<memtkx/MemTKXDSL.hpp>` |
| C++ namespaces | `MemTKX`, `MemTKX::dsl` |
| CMake package | `MemTKX` |
| CMake targets | `MemTKX::MemTKX` (C library), `MemTKX::MemTKXCpp` (C++ wrapper) |
| pkg-config | `memtkx` |
| CMake minimum | 3.20 |
| Package version | 2.0.0 |
| Dependencies | C standard library and platform threads (POSIX threads or Win32) |

## What you can build

- **Short-lived arenas:** use `mtkx_bump_allocator` or `mtkx_bump_space` for
  fast linear allocation and whole-region reuse. The inline
  `mtkx_bump_allocator_try_allocate()` fast path costs about as much as a
  hand-written bump.
- **Individually reclaimable heaps:** use `mtkx_free_list_allocator`,
  `mtkx_free_list_space`, or `mtkx_segregated_allocator`.
- **Reachability-based collection:** use `mtkx_mark_sweep_plan` with roots
  and an object-layout-aware trace callback.
- **Collector experiments:** study copying with `mtkx_semi_space_plan`, line
  marking with `mtkx_immix_plan`, and nursery promotion with
  `mtkx_generational_plan`. All four plans also work through the polymorphic
  `mtkx_plan` interface.
- **Reference-write instrumentation:** compose range filters, record
  modified slots, dirty cards, or capture old references in a SATB queue.
- **Runtime collection orchestration:** register root providers, coordinate
  cooperative safepoints across threads, and run named collection phases.
- **Application plugins:** implement your own spaces and plans as C structs
  with vtables (`mtkx_space_ops`, `mtkx_plan_ops`), or in C++ with the
  original `SpaceBase<Derived>` / `PlanBase<Derived>` extension points.

Allocation returns raw addresses. Your application defines object layouts,
initializes objects, enumerates references, and chooses when collection runs.
MemTKX does not replace `malloc`, scan native stacks automatically, or
trigger a collection when an allocation fails.

## Build and run the tests

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The suite has 27 tests. Sixteen are C tests: ports of the original nine,
plus DSL, plugin, and fault-injection coverage. Eleven are C++ tests: the
original nine C++ tests compiled against the wrapper, plus interop and DSL
tests. The C tests use their own `CHECK` macro and the C++ tests keep
`assert` enabled, so a Release build also checks behavior.

### CMake options

| Option | Default | Purpose |
| --- | --- | --- |
| `MemTKX_BUILD_TESTS` | `ON` | Build and register the test suite |
| `MemTKX_BUILD_BENCHMARKS` | `OFF` | Build the two CSV microbenchmarks |
| `MemTKX_BUILD_EXAMPLES` | `OFF` | Build the programs in `examples/` |
| `MemTKX_ENABLE_WARNINGS` | `ON` | Compile MemTKX and its tests with strict warnings |
| `MemTKX_BUILD_CXX_WRAPPER` | `ON` | Provide and install the C++ wrapper (enables the CXX language) |
| `MemTKX_INSTALL` | `ON` when top-level | Generate install rules |
| `BUILD_SHARED_LIBS` | `OFF` | Build `libmemtkx` as a shared library |

Warnings are private to MemTKX's own targets. They are not pushed onto
your project.

## Add MemTKX to your application

### Use a source checkout

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyRuntime LANGUAGES C)

set(MemTKX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MemTKX_BUILD_CXX_WRAPPER OFF CACHE BOOL "" FORCE)  # C-only project
add_subdirectory(third_party/memtkx)

add_executable(my_runtime main.c)
target_link_libraries(my_runtime PRIVATE MemTKX::MemTKX)
```

C++ applications link `MemTKX::MemTKXCpp` instead. It brings the C library
along and requires C++17.

### Use an installed package

```sh
cmake -S . -B build-install -DMemTKX_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-install --parallel
cmake --install build-install
```

```cmake
find_package(MemTKX 2 CONFIG REQUIRED)
target_link_libraries(my_runtime PRIVATE MemTKX::MemTKX)      # C
target_link_libraries(my_cpp_runtime PRIVATE MemTKX::MemTKXCpp)  # C++
```

The package resolves `Threads` itself. A pkg-config file is installed too:
`cc main.c $(pkg-config --cflags --libs memtkx) -pthread`.

### Compile directly

Link a CMake-built `libmemtkx.a`:

```sh
cc -std=c99 -Iinclude main.c build/libmemtkx.a -pthread -o my_runtime
```

Or compile the library sources straight into your build (POSIX shown):

```sh
cc -std=c99 -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc -c src/*/*.c
ar rcs libmemtkx.a *.o
```

## Your first managed object

Save this complete program as `main.c`:

```c
#include <memtkx/memtkx.h>

#include <stdio.h>

typedef struct node {
  mtkx_address next;
  int value;
} node;

static bool trace_node(mtkx_address object, mtkx_address_vec *children, void *ctx) {
  const node *current = mtkx_from_address(const node, object);
  (void)ctx;
  return current->next == 0 || mtkx_address_vec_push(children, current->next) == MTKX_OK;
}

int main(void) {
  mtkx_mark_sweep_plan *heap = mtkx_mark_sweep_plan_create(MTKX_KB(64));
  mtkx_alloc_result allocation;
  mtkx_address root;
  node *n;
  mtkx_plan_stats_result collected;

  if (heap == NULL) {
    return 1;
  }
  allocation = mtkx_mark_sweep_plan_allocate(heap, sizeof(node), MTKX_ALIGNOF(node));
  if (mtkx_alloc_result_is_err(allocation)) {
    fprintf(stderr, "%s\n", mtkx_alloc_error_string(allocation.error));
    mtkx_mark_sweep_plan_destroy(heap);
    return 1;
  }

  root = mtkx_alloc_result_unwrap(allocation);
  n = mtkx_from_address(node, root);
  n->next = 0;
  n->value = 42;

  collected = mtkx_mark_sweep_plan_collect(heap, &root, 1, mtkx_trace_make(trace_node, NULL));
  if (mtkx_plan_stats_result_is_err(collected)) {
    mtkx_mark_sweep_plan_destroy(heap);
    return 2;
  }
  printf("value=%d, live_objects=%zu, collections=%zu\n", n->value,
         collected.value.live_objects, collected.value.collection_count);

  collected = mtkx_mark_sweep_plan_collect(heap, NULL, 0, mtkx_trace_make(trace_node, NULL));
  mtkx_mark_sweep_plan_destroy(heap);
  return collected.ok ? 0 : 3;
}
```

Expected output:

```text
value=42, live_objects=1, collections=1
```

The root keeps the object reachable. The trace callback appends each
outgoing reference to `children`. With an empty root set, every object
becomes unreachable. `mtkx_mark_sweep_plan` keeps survivors at their
addresses and returns unreachable storage to its free list.

The same program written against the C++ wrapper is the original README
example, unchanged; see [GUIDE.md chapter 14](GUIDE.md#chapter-14--the-c-wrapper).

## Choose an allocator or space

| Component | Owns backing storage? | Reuse behavior |
| --- | --- | --- |
| `mtkx_bump_allocator` | No | Whole-region `reset` |
| `mtkx_bump_space` | No | Whole-region reset; space metadata and metrics |
| `mtkx_free_list_allocator` | No | `free(address, bytes)` and adjacent-cell coalescing |
| `mtkx_free_list_space` | No | `deallocate(address, bytes)` with metrics |
| `mtkx_segregated_allocator` | No | Eight region partitions selected by request size |
| `mtkx_immix_space` | Yes | Block allocation, line marking, whole-block reset |
| `mtkx_los_space` | No | Threshold-filtered bump allocation; reset for reuse |

Allocators and spaces are plain structs you place anywhere. Each has an
`*_init` and, when it owns heap memory, a matching `*_destroy`. Keep
borrowed storage alive, and at a stable address, for the allocator's whole
lifetime.

## Choose a collection plan

| Plan | Collection behavior | Integration detail |
| --- | --- | --- |
| `mtkx_mark_sweep_plan` | Traces reachable objects and frees unreachable ones | Non-moving |
| `mtkx_semi_space_plan` | Copies survivors between two equal-sized regions | Forwarding is not exposed; roots and fields are not rewritten |
| `mtkx_immix_plan` | Traces objects and marks occupied lines | Does not sweep or recycle dead lines |
| `mtkx_generational_plan` | Copies reachable tracked objects into mature storage and resets the nursery | No root/field rewriting, card scanning, or mature sweep |

Plans are opaque objects (`*_create` / `*_destroy`). Each converts to the
polymorphic `mtkx_plan *` with `*_as_plan()`, so you can choose a plan at
run time. A trace callback that returns `false` stops the collection with
`MTKX_GC_INVALID_TRACE`.

## Plugins and extension points

| Extension point | C mechanism | C++ wrapper mechanism |
| --- | --- | --- |
| Space | struct whose first member is `mtkx_space`, plus `mtkx_space_ops` | `SpaceBase<Derived>::allocate_impl` |
| Plan | struct whose first member is `mtkx_plan`, plus `mtkx_plan_ops` | `PlanBase<Derived>::collect_impl` |
| Object scanning | `mtkx_trace` (function + context) | `TraceFn` |
| Root discovery | `mtkx_root_provider_fn`, `mtkx_stack_scan_fn` | `RootProvider`, `StackScanCallback` |
| Collection phase | `mtkx_phase` (function + context) | `make_phase()` |
| Barrier stage | `mtkx_barrier_custom()` | `dsl::pipe` stages |
| Allocator hooks | `mtkx_set_allocator()` | (shared with C) |

Spaces and plans written in C++ are reachable from C through `c_space()` and
`c_plan()`. Exceptions thrown by C++ callbacks never unwind through C. The
wrapper catches them and rethrows them in the C++ caller.

## Benchmarks

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DMemTKX_BUILD_TESTS=OFF -DMemTKX_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/benchmarks/bench_alloc
./build-bench/benchmarks/bench_barrier
```

Both print CSV in nanoseconds per operation. `bench_alloc` reports the
exported bump function and the inline fast path separately. A call into a
compiled library cannot be inlined, while the original header-only
allocator always was. See [chapter 15](GUIDE.md#chapter-15--testing-performance-and-maintenance)
before drawing conclusions from the numbers.

## Repository map

```text
include/memtkx/
  memtkx.h            C umbrella header
  dsl.h               C DSL umbrella header
  core/ dsl/ space/ barrier/ scheduler/ plan/   C headers (*.h)
  MemTKX.hpp          C++ wrapper umbrella (same layout, *.hpp)
  MemTKXDSL.hpp       C++ DSL wrapper umbrella
  detail/             C++ wrapper internals
src/                  C99 implementation (internal/ holds private helpers)
cmake/                Package configuration and pkg-config templates
tests/c/              C test suite
tests/cpp/            Original C++ tests + wrapper interop/DSL tests
benchmarks/           CSV microbenchmarks (C)
examples/             Complete example programs
README.md, GUIDE.md   Overview and manual
```

## Moving from MemTKX 1.x (header-only C++)

* **C++ users:** keep including `<memtkx/MemTKX.hpp>`. Link
  `MemTKX::MemTKXCpp` (or `MemTKX::MemTKX` plus C++17) and ask
  `find_package` for version 2. The API is source compatible apart from
  the small list in [GUIDE.md §14.6](GUIDE.md#146-differences-from-the-header-only-version).
* **New C users:** start at [chapter 1](GUIDE.md#chapter-1--identity-and-architecture).
  [§15.6](GUIDE.md#156-c-to-c-name-map) maps every C++ name to its C
  counterpart.
