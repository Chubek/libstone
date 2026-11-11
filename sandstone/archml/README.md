# ArchML

ArchML compiles version-1 architecture descriptions into reusable code-generator
data and standalone C++20 encoding libraries. The 17 profiles in [`stdspec/`](stdspec/)
contain 634 concrete instruction forms and 23 text/IR operations.

## Build and run

From the repository root:

```sh
cmake -S sandstone/archml -B build-archml -DCMAKE_BUILD_TYPE=Release
cmake --build build-archml -j
ctest --test-dir build-archml --output-on-failure

build-archml/archml --check sandstone/archml/stdspec/riscv64.aml
build-archml/archml --target cpp -o riscv64.hpp sandstone/archml/stdspec/riscv64.aml
build-archml/archml --target json -o riscv64.json sandstone/archml/stdspec/riscv64.aml
build-archml/archml --target archml -o normalized.aml sandstone/archml/stdspec/riscv64.aml
```

Requirements: CMake 3.20+, Perl 5.14+, C11 and C++20 compilers; tests also use
Python 3. ArchML is included in the Sandstone build with
`SANDSTONE_BUILD_ARCHML=ON` (default). `SANDSTONE_BUILD_TOOLS` controls the CLI,
and `SANDSTONE_BUILD_TESTS` controls tests. No external parser library is required.

`--list-targets` lists the available backends. The default is `cpp`.
`--namespace NAME` sets a C++ namespace; otherwise it is derived from the profile
name (`archml_riscv64`, for example). Input/output `-` selects stdin/stdout.
`--check` validates without generating output. Diagnostics include source path,
one-based line and byte column. Output generation completes before opening the
output file, so validation failures preserve existing output.

## Generated C++

```cpp
#include "riscv64.hpp"
#include <array>

int main() {
    const std::array<std::uint64_t, 3> operands{1, 2, 3};
    auto bytes = archml_riscv64::encode("add", operands); // b3 00 31 00
    archml_riscv64::runtime::Decoded decoded;
    return archml_riscv64::decode(bytes, decoded) ? 0 : 1;
}
```

Generated headers are standalone: they need only the C++20 standard library.
Multiple architecture headers can be included together. Each namespace provides:

* `profile`, `instructions`, and `find(name)` for querying lowered forms;
* `encode(name, operands)` returning bytes or throwing `std::invalid_argument`;
* `decode(bytes, result)`, consuming the first matching form in source order;
* `render(name, operand_strings, context)` for assembly/PTX/LLVM IR templates;
* `metadata_json`, retaining the complete profile, including register aliases,
  ABI, features, types, selection, legalization, scheduling, P-code and extensions.

Operands use architectural encoding numbers, in declaration order. Signed
values use their `uint64_t` two's-complement representation, for example
`static_cast<std::uint64_t>(-4)`. Unsigned immediates support all 64 bits.
Displacements are byte-valued; the encoder checks scale, signed/unsigned bounds,
register-class membership and explicit exclusions before emitting bytes.

The runtime treats `base` bytes as already ordered for the target. Field bit zero
is the least significant bit of byte zero, regardless of architecture endianness.
Decoding checks fixed bits and operand restrictions, sign-extends and rescales
operands, and leaves its output unchanged when no form matches. For a particular
form, use `runtime::decode_one`. Runtime descriptors must originate from a
validated document or generated tables.

Rendering takes already formatted operand strings. Presentation bindings such
as a branch label `{target}` are supplied in the optional context map:

```cpp
const std::array<std::string, 3> operands{"x1", "x2", "-4"};
auto text = archml_riscv64::render("beq", operands, {{"target", "loop"}});
// beq x1, x2, loop
```

NVPTX and DirectX operations expose templates and metadata. Their `base` is empty,
encoding them raises an error, and the binary decoder skips them. Consumers
provide the surrounding PTX/DXIL module and value declarations.

Feature/mode selection, prose constraints, P-code execution, instruction
selection, register allocation, scheduling, relocation and ABI lowering belong
to the consuming compiler. Explicit feature/mode dependencies are retained in
the generated descriptors. Structured metadata remains available to implement
those compiler passes; generated encoding functions perform concrete form checks.

## Library interfaces

Link `Sandstone::ArchML` (alias of `sandstone-archml`) in a CMake build.

```cpp
#include <ArchML.hpp>
#include <Target.hpp>

auto doc = sandstone::archml::Document::load("riscv64.aml");
const auto *abi = doc.find("abi", "lp64d");
const auto *form = doc.instruction("add");
auto source = sandstone::archml::generate(doc, "cpp", {"my_riscv"});
```

`Document::parse(text, source_name)` reads memory. Documents own their trees and
are copyable/movable. Query pointers remain valid until the owning document is
destroyed, moved or assigned. Concurrent parsing is supported; the Aurocks parser
is serialized internally, while validation and immutable document queries are
independent. `Error` carries a `Location` and a formatted diagnostic.

[`ArchML.h`](ArchML.h) is the C interface. It provides load/parse, document
lifetime, record lookup as JSON, generation, encoding and decoding. Errors are
caller-owned; generated strings are released with `sandstone_archml_free`.
Encode/decode accept caller buffers, report required sizes, and preserve buffers
on failure. See [`tests/CAPI.c`](tests/CAPI.c) for a complete example. Instruction
enumeration includes text-only operations.

Installation places the library in `lib`, headers in `include/sandstone/archml`,
the CLI in `bin`, and profiles in `share/sandstone/archml/stdspec` (subject to
CMake's standard installation-directory settings).

## Syntax, AST and validation

```lisp
; Comments extend to the end of the line.
(archml 1 example
  (target example (bits 64) (endian little))
  (pcode-op nop (arity 0 0))
  (instruction nop
    (operands)
    (encoding (base "90"))
    (pcode (nop))
    (assembly "nop")))
```

Source and strings are UTF-8. Symbols are unquoted atoms; strings support `\"`,
`\\`, `\n`, `\r`, `\t`, `\b`, `\f` and two-digit `\xHH` byte escapes. Escaped
strings must also be valid UTF-8; `\x00` is permitted inside strings. Raw NUL,
trailing input, malformed strings and excessive nesting are diagnosed.

`ArchML.g` is compiled by `../../scripts/aurocks.pl`; `ArchML.asdl` is compiled by
`../../scripts/asdl.pl`. CMake generates both in the build directory. The ASDL
arena owns the document and source-offset-bearing symbol/string/list nodes,
including abandoned parse alternatives. A C bridge keeps generated C11 AST
implementation details out of the C++ API. Aurocks runtime symbols are prefixed
to permit linking other generated parsers into the same application.

The semantic pass checks the version and target, unique records/properties,
references and feature cycles, register storage bounds, operand declarations,
P-code operators/arities/operand references, assembly placeholders, and encoding
field bounds, coverage and overlaps. Unknown named metadata records and
properties remain in the tree for downstream compiler extensions.

The JSON backend represents lists as arrays, strings as JSON strings and symbols
as `{"symbol":"spelling"}`. Numeric and boolean atoms preserve their original
spelling, avoiding precision loss and keeping symbols distinct from strings.
The `archml` backend writes canonical S-expressions with the same tree and order;
comments and original whitespace are omitted.

## Extending generation

Implement the `Target` interface in [`targets/`](targets/) and register the
factory in `targets/Target.cpp`. A backend receives the validated `Document`,
an output stream, and `TargetOptions`. `targets/Cpp.cpp` emits standalone code;
`targets/Data.cpp` implements JSON and canonical ArchML output. The embedded C++
runtime comes from `Runtime.hpp`, shared with the library to keep both paths
consistent.

Tests load every shipped profile, roundtrip metadata and all forms with boundary
and randomized operands, check independently specified machine bytes, exercise
invalid inputs and C ownership/buffer contracts, and compile/run all generated
headers together with strict compiler warnings.
