# qwasm

`qwasm` is a dependency-free C++20 library and optional command-line tool for
read-only inspection of core WebAssembly binary modules. It does not compile,
instantiate, execute, or rewrite modules.

```sh
qwasm module.wasm
qwasm -- -module.wasm
```

The summary lists sections in file order, with byte offsets and payload sizes.
It includes custom section names and data ranges, import descriptors, export
kinds and indices, and any start function index or data count. The CLI quotes
names and paths: `"` and `\` are escaped, and each byte outside printable ASCII
is shown as `\xHH` (two hex digits). Exit status is `0` for a summary or help,
`1` for an inspection or I/O error, and `2` for an invalid command line.

## What is checked

- The core Wasm version-1 preamble, section sizes and boundaries, and the
  order and uniqueness of known standard sections. Custom sections can appear
  more than once and anywhere. The tag section (ID 13) follows memory; the
  data-count section (ID 12) precedes code.
- UTF-8 names in custom, import, and export sections. Empty names and embedded
  NUL bytes are preserved. The bytes after a custom section name are opaque.
- Import and export record counts, encodings, and exact payload consumption.
  Imports cover functions, tables, memories, globals, and tags. Table references
  support `funcref` and `externref`; globals also support scalar numeric types
  and `v128`. Limit fields are unsigned 64-bit LEB128 as in the current core
  binary grammar; both address widths and the shared memory flag are reported.
- The start index and data-count scalar encodings. LEB128 fields may be padded
  to their declared width; overlong or overflowing encodings are rejected.

**A successful summary is not validation.** All other standard sections are
opaque beyond their framing and the presence of a payload. The reader does not
decode instructions or validate indices, types, limits, section-to-section
counts, or the behavior of exported functions. For example, an exported index
is reported verbatim even if no corresponding definition exists.

| API result | Meaning |
| --- | --- |
| `Module` | Metadata in the supported subset could be read. |
| `ErrorKind::malformed` | A framing or decoded field check failed. |
| `ErrorKind::unsupported` | Non-core-v1 preamble (including components), unknown section ID, or a recognized import encoding outside the subset: richer reference types, shared tables, or custom memory page sizes. |
| `ErrorKind::limit_exceeded` | Input exceeded a configured resource ceiling. |

Errors include a zero-based file offset, which can be the end of a section or
file when bytes are missing, and a diagnostic that does not echo input bytes.
Inspection stops at the first reported error; unsupported or over-limit input
is not inspected further.

The default limits are 64 MiB per module, 16,384 sections, 65,536 imports,
65,536 exports, and 65,536 UTF-8 bytes per name. The CLI uses these defaults;
library callers can supply `qwasm::InspectionLimits`.

## C++ library

Link `qwasm::qwasm` and include `<qwasm/qwasm.hpp>`:

```cpp
#include <qwasm/qwasm.hpp>
#include <iostream>

bool summarize(std::span<const std::uint8_t> wasm) {
    auto result = qwasm::inspect(wasm);
    if (const auto* err = std::get_if<qwasm::Error>(&result)) {
        std::cerr << qwasm::to_string(err->kind) << " at byte " << err->offset
                  << ": " << err->message << '\n';
        return false;
    }
    const auto& module = std::get<qwasm::Module>(result);
    std::cout << module.sections.size() << " sections\n";
    return true;
}
```

Results own their decoded names; byte ranges are offsets into the original
module, not borrowed pointers. Keep the source bytes if you want to inspect an
opaque range. Names are not escaped by the library; escape them before
displaying them. Allocation failures can propagate as C++ exceptions.

## Build and test

Yellowstone always builds the library and uses `YELLOWSTONE_BUILD_TOOLS` and
`YELLOWSTONE_BUILD_TESTS` to select the `qwasm` executable and regression tests.
The component also builds independently (CMake 3.20+, C++20 compiler):

```sh
cmake -S yellowstone/qwasm -B build-qwasm \
  -DYELLOWSTONE_BUILD_TOOLS=ON -DYELLOWSTONE_BUILD_TESTS=ON
cmake --build build-qwasm
ctest --test-dir build-qwasm --output-on-failure
```

Use `find_package(Yellowstone CONFIG REQUIRED)` for a Yellowstone install or
`find_package(qwasm CONFIG REQUIRED)` for a standalone component install.
Both provide the `qwasm::qwasm` target.
