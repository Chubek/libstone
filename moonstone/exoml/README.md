# ExoML — declarative static bindings

ExoML provides **compiler-checked native auto-binding**, generated C++ wrappers,
ABI checks/probes, declaration reflection, a generated DSL frontend, and an
in-memory StaticFFI standard library. See [STATUS.md](STATUS.md) for remaining work.

## Build

```sh
cmake -S moonstone -B build-moonstone -DMOONSTONE_BUILD_TESTS=ON
cmake --build build-moonstone --parallel
ctest --test-dir build-moonstone --output-on-failure
```

Requires C++20, C11, CMake 3.20 and Perl (core modules only). Repository
`scripts/aurocks.pl` and `scripts/asdl.pl` generate the parser/AST offline. Python
enables the additional compiler-rejection/ABI JSON test. Disable
`MOONSTONE_BUILD_PROLIFIC` when its unrelated dependencies are unavailable.
Link **`Moonstone::ExoML`** (`Moonstone::Exoml` also works in the build tree).
ExoML now needs its compiled runtime; merely including headers is insufficient.
The existing command, value and declaration APIs remain available.

After `cmake --install build-moonstone --prefix PREFIX`, external CMake consumers
can use `find_package(MoonstoneExoML CONFIG REQUIRED)` and link
`Moonstone::ExoML`. Point `CMAKE_PREFIX_PATH` at PREFIX. Installed consumers do
not need Perl or the source tree.

## Native auto-binding

```cpp
#include <exoml/StaticFFI.hpp>
int add(int a, int b) { return a + b; }

moonstone::exoml::Interpreter vm;
moonstone::exoml::bind_native(vm, "Math:Add", &add);
vm.enable_hatch(true);
vm.allow_service("Math:Add");
auto errors = vm.execute(moonstone::exoml::Parser(
    "Answer ~hatch! Math:Add(20, 22);").parse());
```

`bind_native` deduces free/noexcept functions, `std::function`, and non-overloaded
lambdas/functors. Overloaded functions need a C++ cast; member functions can be
adapted with a lambda that owns/pins the object.

| Native type | Conversion contract |
| --- | --- |
| Signed/unsigned integer through 64 bits | Range checked; booleans/reals are not integers |
| `bool` | Boolean only |
| `float`, `double` | Finite real or integer in ±2^53; float rounding permitted |
| Enum | Underlying integer range checked; enumerator membership is host policy |
| `std::string`, `const std::string&` | Binary-safe copied string |
| `std::string_view` | View valid during call; returned views copied |
| `const char*` | Explicit `NativeOptions::c_strings` promise; NUL-free input, copied output |
| `std::optional<T>` | Null maps to absence, otherwise T's conversion |
| `std::vector<std::string>` | String list |
| `Value` | Explicit generic value adapter |
| `void` result | Null |

C-string results have a default 1 MiB scan budget; null pointers become null.
Input pointers/views must not be retained. Other pointers, mutable references,
output buffers, variadic calls and consuming/escaping ownership require explicit
host adapters. All arguments are checked before invocation. Native exceptions
become diagnostics; native side effects cannot be rolled back.

Native calls default to privileged and unknown effects. A trusted host can set
`NativeOptions::privileged = false` for ordinary commands. `thread_affine = true`
restricts invocation to the registration thread. Calls synchronously block the
caller thread; there is no automatic scheduling/cancellation/migration.
Interpreter, library and workspace mutation require external synchronization.

## Whole-library binding

```cpp
using namespace moonstone::exoml;
StaticLibrary math("Math");
math.bind("Add", &add).alias("Sum", "Add").constant("Answer", std::int64_t(42));
AutoBindOptions selection;
selection.name_space = "App:Math";
selection.include = {"Sum", "Answer"};
selection.rename = {{"Sum", "Plus"}};
math.install(vm, selection);
```

Installation is all-or-nothing. Unknown selections, duplicate/renamed collisions
and existing-service collisions fail. Constants are public zero-argument
services. `exports()`/`services()` expose signatures, arity, documentation,
ownership/effects and affinity. `snapshot()` copies VM symbols. The C++ host and
its callback access are trusted, not a sandbox boundary.

## Binding generation and ABI

[examples/fixture.exo](examples/fixture.exo) is a complete runnable example using
[tests/fixture.h](tests/fixture.h), backed by real C code:

```sh
build-moonstone/exoml/exoml --emit Source moonstone/exoml/examples/fixture.exo > bindings.hpp
build-moonstone/exoml/exoml --emit Probe moonstone/exoml/examples/fixture.exo > probe.cpp
build-moonstone/exoml/exoml --emit Manifest moonstone/exoml/examples/fixture.exo
```

Compile the generated header into your application, link its C library and
ExoML, then call `exoml_generated::bind_Script(vm)`. Generated code includes real
headers and asserts exact function-pointer types. Typedefs resolve in the
compiler; ExoML does not guess ABI widths.

The C++ `BindingModule` API includes exported names, constants, contracts,
source locations and expected record layouts:

- `binding_diagnostics`: structured errors; `validate_binding`: legacy strings.
- `generate_bindings`: C++20 header with an atomic module installer.
- `generate_abi_checks`: compilable signature/layout assertions.
- `generate_abi_probe`: C++20 program reporting actual `sizeof`, `alignof`,
  `offsetof`, and pointer size as JSON. Compile/run with the **target toolchain**.
- `describe_binding`: deterministic reflection JSON, including source/contracts.
- `StaticFFI::add/install`: immutable copies of host-supplied modules.
- `install_static_ffi`: mutable declaration workspace and Types helpers.

Accepted type spellings are built-in scalars, typedef names, tagged types,
`const`/`volatile` and pointer stars. This is not a C header parser. Complex
declarators need a typedef. Known unsupported conversions fail generation;
unsupported typedef targets fail C++ template compilation. ABI checks/probes can
inspect records; fields must be addressable and records standard-layout.
Bitfields fail compilation. Expected layouts become evidence only after an
independent target compilation. JSON reflection is not a manifest-input parser.

## Language and CLI

```exoml
M ~setconst! StaticFFI:Module("Demo");
F ~get! StaticFFI:Function($M, "add", "int", ["int", "int"]);
Source ~get! StaticFFI:Generate($M);
Kind ~get! Types:Name($@);
```

Commands are `binding operator Service:Name(arguments);`. `~setconst!` creates
an immutable binding; `~setvar!` and `~get!` create assignable bindings;
`~hatch!` explicitly invokes privileged services. `$name` resolves a binding;
`$@` resolves the last successful result, preserved across failures. `--` starts
a line comment. Arguments: signed decimal integers, `u`-suffixed unsigned
integers, finite decimal/exponent reals, booleans, null, strings, string lists,
and references. Escapes: `\n`, `\r`, `\t`, `\0`, `\"`, `\\`.

Locations use file/line/byte-column spans ending just after `;`. Parsing defaults
to 1 MiB input, 4096 commands and 128 arguments/call (`ParseLimits`). Aurocks
global parser state is serialized. The VM validates caller-built commands and
has a separate command budget. `execute` collects errors by default;
`ExecutionOptions::stop_on_error` enables fail-fast behavior, used by the CLI.

`exoml --check FILE` parses only; `--emit SYMBOL` prints one result verbatim;
`-` reads stdin. `--repl` accepts one complete module per line, with `:symbols`
and `:quit`. Declarative services never read headers, write files, run compilers
or load native code. See the [stdlib reference](stdlib/README.md).
