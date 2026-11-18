# binrev: static binary inspection

binrev is a read-only C++20 library for diagnosing binary layouts and identifying
which section and symbol a reported location belongs to. It uses qBFD to parse
ELF, PE, thin Mach-O, and standalone COFF objects. It does not load or execute
the input, apply relocations, or modify the file.

## Command line

Build the `binrev_cli` target with `YELLOWSTONE_BUILD_TOOLS=ON`; the executable
is named `binrev`. With no options it prints format, architecture, and image
identity. `--sections`, `--symbols`, and `--relocations` can be combined.

```sh
binrev --sections --symbols --relocations sample.elf
binrev --offset 0 --bytes 16 sample.exe        # Inspect the file header.
binrev --address 0x140001000 sample.exe        # Use a VA, not the RVA 0x1000.
binrev --section 1:0 --bytes 8 sample.o       # Use an index from --sections.
```

Use at most one of `--address VA`, `--offset OFF`, or `--section INDEX:OFF`.
Numbers are unsigned decimal or `0x`-prefixed hex. `--bytes` requires a
location and displays 1–4096 bytes. Exit status is `0` on success, `1` on
input/query failure, and `2` for invalid arguments.

## C++ API

In the source tree or with `find_package(Yellowstone CONFIG REQUIRED)`, link to
`binrev::binrev`. The public header is `<binrev/BinRev.hpp>`. `Binary::open`
accepts a filesystem path or an owned byte vector and returns a `qbfd::Expected`.

```cpp
#include <binrev/BinRev.hpp>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  auto file = binrev::Binary::open(argv[1]);
  if (!file) {
    std::cerr << file.error().message << '\n';
    return 1;
  }
  auto at = file->locate(binrev::Address{file->object().entryPoint()});
  if (!at) {
    std::cerr << at.error().message << '\n';
    return 1;
  }
  if (at->symbol)
    std::cout << at->symbol->symbol.name << '+' << at->symbol->displacement << '\n';
}
```

`Binary::object()` exposes qBFD's canonical sections, static and dynamic
symbols, and per-section relocations (via `relocations(section)`). `locate()`
returns an owned `Location`; `readBytes()` and `object()` borrow from the
`Binary`, which must outlive them.

- `Address` is the preferred, linked virtual address. It must be inside an
  allocated section. Relocatable objects have no linked address space; use
  `SectionOffset` with the canonical section index, not a section name.
- `FileOffset` is a zero-based offset into the input. `locate()` can return just
  that offset (for example, in a header, gap, or PE raw padding); `readBytes()`
  at a file offset does not require section attribution.
- A location in zero-fill storage or a PE virtual tail has no file offset.
  Address and section reads must fit in one file-backed section. Reads are
  nonempty and fail rather than clip. Overlapping mappings are ambiguous when
  attributing an address or file offset; explicit section offsets identify
  the desired section independently.
- Attribution tests symbols defined in the selected section against their
  reported extent. Zero-size symbols match exactly; direct PE exports
  without a section association also match only exactly. When more than
  one symbol matches, the closest start wins, with ties in qBFD order and
  static before dynamic. Undefined, imported, absolute, debug, file,
  section, common and TLS symbols are excluded. There is no nearest-symbol
  guess beyond a symbol's known extent.

## Limits and tests

Address queries use sections, not loader mappings: unmapped addresses,
segment-only images, and virtual image headers are not mapped by this API.
Supply a separate thin slice for universal Mach-O. Archive traversal,
runtime rebasing, disassembly, and source-line lookup are not included.
Metadata completeness reflects the underlying qBFD reader; for example,
records qBFD cannot associate with a section are not listed as relocations.
Path inputs must be readable regular files. Inputs are buffered in memory and
limited to 256 MiB (`binrev::maxInputBytes`) on both open paths. This caps input
size, not the memory needed to parse or list the resulting metadata.

With `YELLOWSTONE_BUILD_TESTS=ON`, run `ctest -R '^binrev_'` from the build
directory. The tests synthesize their objects and require no cross-toolchain.
