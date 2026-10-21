# 5. Embedding and API contracts

## Choose a component interface

There is no universal Libstone handle type, error convention, or ownership rule.
Link the component you actually use and follow that component's public contract.

| Component | Source-tree CMake target | Installed consumption |
| --- | --- | --- |
| Firestone | `Firestone::firestone`, `Firestone::ir`, `Firestone::eqsat`, etc. | `find_package(Firestone CONFIG REQUIRED)` |
| Blackstone | `Blackstone::blackstone` | `find_package(Blackstone CONFIG REQUIRED)`; also resolves Firestone |
| Flintstone | `Flintstone::flintstone`, `Flintstone::native` | `find_package(Flintstone CONFIG REQUIRED)` |
| Limestone | Targets such as `limestone_core`, `regtl`, `schedrow` | `find_package(Limestone CONFIG REQUIRED COMPONENTS core il)`; targets such as `Limestone::core` |
| Sandstone | `sandstone`, historical alias `domsymas` | Installed archive and headers; no standalone exported CMake package |
| XBind | `XBind::xbind`, `XBind::QaMRppSpec` | Installed headers/libraries; do not assume a `find_package(XBind)` config exists |

For source-tree integration, add the needed component with `add_subdirectory`.
Avoid adding it twice: Flintstone can add sibling dependencies itself. Tests and
tools can be disabled before adding a component to keep a host build focused.

## A complete Firestone consumer

The runnable example in [examples/firestone](examples/firestone) uses equality
saturation to reduce `(add (mul x 1) 0)` to `x`.

After [installing Firestone](02-building.md#installation):

```sh
cmake -S manual/examples/firestone -B /tmp/opencode/libstone-manual-example \
  -DCMAKE_PREFIX_PATH=/tmp/opencode/libstone-prefix
cmake --build /tmp/opencode/libstone-manual-example --parallel 2
/tmp/opencode/libstone-manual-example/manual-firestone
# x
```

Its CMake integration is:

```cmake
find_package(Firestone 0.1 CONFIG REQUIRED COMPONENTS eqsat)
target_link_libraries(manual-firestone PRIVATE Firestone::eqsat)
```

The [complete source](examples/firestone/main.cpp) parses terms, installs two
caller-supplied equalities, runs bounded saturation, and extracts a term. Those
equalities are assertions made by the application. They are not automatically
valid for arbitrary floating-point, overflow, or effect semantics.

Firestone's public headers live below its include root, for example
`<libir/SimpleSSAGraph.hpp>` and `<libeqsat/Saturation.hpp>`. Link exported targets
to receive the correct include paths and C++17 requirement. The [API guide](../firestone/docs/API.md)
describes construction and verification; the [VM guide](../firestone/docs/VM.md)
describes bytecode loading, execution, and backend callbacks.

## Blackstone

Installed C++ hosts use `<blackstone.hpp>` and
`Blackstone::blackstone`. A host with an existing valid Firestone SSA graph can
load and run a kernel:

```cpp
// graph is a caller-supplied firestone::ir::SimpleSSAGraph.
blackstone::Runtime runtime;
runtime.load_directory("/path/to/blackstone/kernels");
auto result = runtime.run("strength_reduction", graph,
                         blackstone::IntegerSemantics::Modular);
```

This is an API fragment, not a standalone program. The installed package exposes
`Blackstone_KERNEL_DIR` so applications can locate installed kernels without
retaining a source-tree pathname.

The Firestone adapter verifies input, runs against an independent snapshot, and
commits only a changed, verified result. A false result or exception leaves the
original graph untouched. Inspect `result.invalidated` before reusing analyses.

The C interface in `blackstone.h` uses an opaque runtime. Operations return zero
on success and `-1` on error; `blackstone_create` can return null. Retrieve errors
through `blackstone_last_error`. The generic C execution callback supplies one
Lua context table and must not throw a C++ exception. Unlike the C++ Firestone
adapter, this generic host interface makes rollback the host's responsibility.

A runtime is single-threaded and non-reentrant. Use separate runtimes for
concurrency. Catalog strings are borrowed until the next successful load or
destruction. Lua kernels have standard libraries available: treat them as
application code, not untrusted sandboxed rewrites.

## Flintstone

Use `<Flintstone.hpp>` for C++ or `<Flintstone.h>` for the C ABI. A typical
installed consumer links:

```cmake
find_package(Flintstone 0.1 CONFIG REQUIRED)
target_link_libraries(my_compiler PRIVATE Flintstone::flintstone)
```

`Flintstone::native` additionally exposes the native-integration representation
headers. Its installed package resolves Sandstone from the same installation;
there is no need to invent a separate Sandstone package name.

The usual C++ sequence is parse, verify/specialize, transform, and execute or
emit. The registry owns dialect/target definitions. Execution works on a private
module and binds Flintstone's VM arithmetic intrinsics. Direct FSVM consumers
must call `flintstone::bind_vm_runtime` themselves.

Host external functions require matching declarations and environment bindings.
Runtime effects on a supplied environment or callback are not undone on later
failure. VM pointer values identify checked heap cells, not native memory.
Consult the [embedding chapter](../flintstone/manual/08-embedding.md) for C handle
lifetimes and the [execution chapter](../flintstone/manual/06-execution.md) for
runtime value kinds and width checks.

## Limestone

The installed package exposes individual libraries rather than vendor
implementation types:

```cmake
find_package(Limestone 0.1 CONFIG REQUIRED COMPONENTS core il)
target_link_libraries(my_backend PRIVATE Limestone::core Limestone::il)
```

Includes retain their source layout, such as `<limestone/limestone.hpp>` and
`<limestone/limestone.h>`. C++ operations return `limestone::Result<T>`; C uses
owning opaque handles and structured diagnostics. Text, instruction strings,
and byte spans returned from a handle are borrowed; do not retain them beyond
their documented owner lifetime. Independent handles can be used independently;
synchronize shared mutable handles.

Exolayer's C symbols use `exl_`. Check native invocation availability when
libffi support is optional. Python's convenience wrappers consume the public C
handles; enabling SWIG does not expose internal solver or parser objects.

See [C/C++ embedding](../limestone/manual/18-c-and-cpp-embedding.md) and
[Python bindings](../limestone/manual/19-python-bindings.md).

## Sandstone and bindings

Sandstone offers C and C++ databank interfaces in `<sandstone.h>` and
`<sandstone.hpp>`. Historical `domsymas` names remain compatibility interfaces;
do not mechanically rename serialized record prefixes. The static `sandstone`
target carries compression/thread link requirements in source-tree builds.

XBind-generated opaque pointers do not acquire automatic lifetime management.
An ownership annotation documents intent; it does not install a Lua finalizer
or prevent duplicate destruction. Call the correct destructor once, invalidate
aliases, and keep owners alive for borrowed pointers. Automatic callbacks,
records, buffer/out-parameter marshalling, and ownership wrappers are not supplied
by the current emitter.

Next: [Targets and extension points](06-extending.md).
