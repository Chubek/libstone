# ExoML — declarative static bindings

ExoML provides **compiler-checked native auto-binding**, generated C++ wrappers,
ABI checks/probes, declaration reflection, a generated DSL frontend, and an
in-memory StaticFFI standard library. See [STATUS.md](STATUS.md) for remaining work.

An **opt-in C11 discovery** library/tool now preserves compiler-derived type
graphs and target evidence. It does not infer ownership or generate safe APIs.
See [C11 discovery](manual/c11-discovery.md) and the
[verification report](manual/c11-discovery-report.md).

A **versioned semantic contract** model attaches ownership, lifetime, error,
effect, callback, destructor and concurrency obligations to discovered
declarations. Contracts are validated against the CDecl IR (Gate B) and
serialized deterministically. See
[semantic contracts](manual/semantic-contracts.md).

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

## C11 discovery (developer option)

Requires installed libclang headers/library and OpenSSL Crypto, only when enabled.
Compilation-database JSON uses the existing vendored nlohmann 3.10.5 header under
`third_party/metatk/third_party/tomlplusplus/vendor`; no dependency is downloaded.
Set `MOONSTONE_EXOML_JSON_INCLUDE_DIR` if that header is provided elsewhere.
Neither the JSON header nor libclang is needed by ordinary runtime consumers.
No vendor trees or runtime dependencies are downloaded or changed.

```sh
cmake -S moonstone -B build-discovery -DCMAKE_BUILD_TYPE=Debug \
  -DMOONSTONE_BUILD_PROLIFIC=OFF -DMOONSTONE_EXOML_EXTRACT=ON
cmake --build build-discovery --parallel 4
ctest --test-dir build-discovery --output-on-failure
build-discovery/exoml/exoml-bind extract \
  --header moonstone/exoml/tests/discovery.h \
  --resource-dir "$(clang -print-resource-dir)" --out discovery.cdecl.json
```

`--include`, `--define`, `--undefine`, `--target`, `--revision` and
`--provenance` record explicit discovery configuration. Specify a resource
directory compatible with libclang for standard headers; cross targets also
need appropriate target include directories. `--help` lists the exact interface.
Only `extract` and `validate` are implemented: invalid CLI syntax exits 2,
source/options/budget failures exit 3 (partial JSON with `complete=false`),
validation failures exit 3, output failures exit 7. Default output is stdout.
No compiler plugins, response files or arbitrary compiler flags are accepted.
Active `MULIB_IMPL_*` definitions are rejected.

C++ clients use `<exoml/Extract.hpp>`, `cdecl::ExtractOptions`, and
`cdecl::extract_c11`, linking `Moonstone::ExoMLExtract`. Installed SDK users request
`find_package(MoonstoneExoMLExtract CONFIG REQUIRED)` separately. The owned model
and deterministic `describe_translation_unit` are in `<exoml/CDecl.hpp>` and
available through the ordinary runtime without libclang/OpenSSL.

`--revision` is caller-supplied provenance, not a checkout-verification claim.
The opt-in MuLIB test verifies an exact pinned clean checkout independently;
enable it with `-DMOONSTONE_EXOML_MULIB_ROOT=/path/to/canonical/mulib` (pin in the
verification report). Normal builds require neither MuLIB nor the extractor.
Fingerprint changes are tested for headers, includes, macro and target options.
The initial pinned MuLIB gate-A discovery milestone is verified; see
[the closeout report](manual/discovery-gate-a.md). Metadata is now discovery
schema **2**, preserving prior fields and adding compilation context, source
tokens and compiler-reported preprocessing evidence. This is not semantic-contract
schema v2 and never promotes an old reflection object into a safe contract.

### Compilation context and macro evidence

```sh
cmake -S moonstone -B build-discovery -DMOONSTONE_BUILD_PROLIFIC=OFF \
  -DMOONSTONE_EXOML_EXTRACT=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-discovery --parallel 4
build-discovery/exoml/exoml-bind extract \
  --header moonstone/exoml/tests/fixture.h \
  --compile-commands build-discovery/compile_commands.json \
  --compile-file moonstone/exoml/tests/fixture.c \
  --resource-dir "$(clang -print-resource-dir)" --out fixture.cdecl.json
```

An exact source entry, matching header entry, or sole entry is required;
ambiguous databases fail. `arguments` arrays and restricted POSIX-quoted `command`
strings are supported, but no database driver, wrapper or shell ever runs.
Only flags configuring C11 preprocessing/ABI are forwarded; output/dependency
actions are stripped **and recorded**. Unknown flags, plugins, response files,
shell expansion, and non-C11 modes fail with diagnostics. The recorded driver is
not an ABI authority: the actual compiler remains the recorded libclang target.
This applies a source entry's flags to the requested header, not source-file
statements preceding that header's inclusion.

`--compile-arg ARG` adds restricted tokenized flags, including `-m32`/`-m64`,
`-fshort-enums`, `-fsigned-char`/`-funsigned-char`, `-fpack-struct=N`, target/CPU/ABI,
sysroot and include options. Database flags come first, then compile arguments,
include directories, defines, undefines and explicit target/resource overrides.
No arbitrary flag passthrough is available; see the closeout report for the list.
SDK equivalents are `ExtractOptions::{compile_commands,compile_file,compiler_arguments}`.

Preprocessing metadata records definitions, source-level expansion/definition
links, includes, skipped ranges and final defined states for observed/CLI names
and standard dynamic builtins. Compiler `#ifdef` probes account for `#undef` and
push/pop; unavailable restored/dynamic definition IDs carry explicit reasons.
Libclang does not expose nested replacement-token trees: these are not claimed
complete or fabricated. Unsupported probes fail the complete-evidence gate while
preserving partial graph/diagnostics. Files must remain stable during discovery.

### Persistent normalization cache (Linux)

```sh
build-discovery/exoml/exoml-bind extract \
  --header moonstone/exoml/tests/discovery.h \
  --resource-dir "$(clang -print-resource-dir)" \
  --cache-dir /tmp/opencode/exoml-discovery-cache --cache-report --out discovery.cdecl.json
# Repeat: stderr reports a hit; declaration JSON is byte-identical.
```

The parent directory must exist; the cache creates a private 0700 leaf directory
and rejects nonprivate/user-foreign roots and symlinks. `--cache-report` adds one
JSON activity line to stderr (separate from deterministic declaration metadata).
Every hit still parses and validates with Clang; only graph normalization is
cached, using fresh AST/input/tool fingerprints. Native cached code is never
loaded. Corrupt entries rebuild atomically; requested cache/storage failures exit
3 with `complete=false`. Cache capacity is not automatically evicted or bounded
across entries; callers manage the directory. The cache assumes a trusted local
user and Linux `/proc`; it is not a shared untrusted artifact store or a sandbox.

`ExtractOptions::cache_directory` and the two-argument
`extract_c11(options, CacheReport&)` expose the same behavior to SDK users.
The old one-argument function remains available. `--max-types`,
`--max-declarations`, `--max-depth`, `--max-input-bytes`, `--max-entries`, and
`--max-cache-bytes` expose extraction budgets. See the
[cache design and verification](manual/discovery-cache.md) for exact guarantees
and remaining P0.1 work. No parse-time or performance improvement is claimed.

### Semantic contract validation

```sh
build-discovery/exoml/exoml-bind validate \
  --decl discovery.cdecl.json \
  --contract example.exocontract.json
# Exit 0: valid; exit 3: validation failures (diagnostics on stderr).
```

The `validate` subcommand reads a discovery JSON (output of `extract`) and a
`*.exocontract.json` sidecar, validates the contract against the CDecl IR, and
reports Gate B violations. `--out FILE` writes deterministic normalized
contract JSON. The contract model, sidecar format and validation rules are in
[semantic contracts](manual/semantic-contracts.md). C++ clients use
`<exoml/Contract.hpp>`: `parse_contract_sidecar`, `validate_contract`,
`serialize_contract`, `project_to_ownership` and `is_safe_for_export`.

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
