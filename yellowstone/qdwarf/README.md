# qdwarf

`qdwarf` is a C++20 library and optional CLI for read-only inspection of DWARF
metadata. qBFD opens ELF, PE images, COFF objects, and thin Mach-O files
(including the binary inside a dSYM bundle). Neither the library nor the CLI
executes the input, opens recorded source paths, or fetches debug information.

```sh
qdwarf program                         # DWARF unit headers
qdwarf --units --lines program         # headers and decoded line rows
qdwarf --address 0x112a program        # source row at a linked address
qdwarf --lines Foo.dSYM/Contents/Resources/DWARF/Foo
```

`--lines` or `--address` alone does not read unit headers. A missing requested
section is reported as absent, not malformed. The CLI returns `0` on success,
`1` on input failure or when an address has no row, and `2` on usage errors.
Use linked virtual addresses, not file offsets, PE RVAs, or runtime addresses
to which a load bias has been applied.

## C++ API

Link `qdwarf::qdwarf` (available with `find_package(Yellowstone CONFIG REQUIRED)`
for an installed LibStone). The CLI and tests are gated by
`YELLOWSTONE_BUILD_TOOLS` and `YELLOWSTONE_BUILD_TESTS`, respectively.

```cpp
#include <qdwarf/qdwarf.hpp>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    qdwarf::Options options;
    options.units = false;
    options.lines = true;
    const auto report = qdwarf::inspect_file(argv[1], options);
    if (const auto match = qdwarf::lookup(report, 0x112a)) {
      const auto& table = report.line_tables[match->table];
      const auto& row = table.rows[match->row];
      if (const auto* file = table.file(row.file))
        std::cout << file->path << ':' << row.line << ':' << row.column << '\n';
    }
  } catch (const qdwarf::Error& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
```

`inspect` accepts a byte span instead of a file path. Both overloads return
owned results. `qdwarf::Error` gives a category and, for DWARF decoder errors,
the section name and section-relative byte offset. The report keeps encoded
file and directory indices; use `LineTable::file` and `LineTable::directory`
to interpret them with the correct version. Paths are returned as recorded;
pre-v5 directory index zero requires the compilation directory, which this
library does not recover from DIEs. Escape record strings if rendering them to
a terminal or another structured format.

Lookup uses half-open address intervals within each sequence, does not bridge
gaps, and chooses the last row at a repeated address before an advance. When
different sequences overlap, it uses the first matching interval in section
order. It does not distinguish instruction slots at the same address; use the
decoded rows and their `op_index` if slot information matters.

## Supported subset

- DWARF 2–5 `.debug_info` and DWARF 4 `.debug_types` **unit headers**, in
  DWARF32 and DWARF64 and either byte order. Body DIEs and abbreviation
  references are not validated or decoded. Header offsets are raw fields;
  unit sections with relocation records are flagged.
- DWARF 2–5 **line programs**, with standard rows, sequence terminators, and
  an address lookup helper. In pre-v5 tables, the `set_address` operand
  supplies the address width; if absent, qdwarf uses the object class. The
  parser supports legacy inline directory/file entries and DWARF 5 paths
  encoded as inline strings or offsets into `.debug_line_str`/`.debug_str`.
  It consumes common integer and block metadata fields without exposing
  them (timestamps, file sizes, MD5, etc.).
- Unknown selected versions, unit types, line opcodes, and used forms are
  diagnosed rather than guessed. Compressed or split DWARF sections,
  segmented line programs, DWARF 5 `define_file`, and line sections with
  relocation records are not supported. Use a linked uncompressed debug
  image without retained debug relocations. For universal Mach-O, extract
  the desired slice first.
- No archive traversal, automatic separate-debug or PDB lookup, inline
  call stacks, DIE/variable/type inspection, range or location expressions,
  or unwind information.

The DWARF decoder checks record boundaries, string termination, LEB128
width, references used for source mapping, and address/line arithmetic.
`Options::limits` controls input bytes, selected section bytes, parsed
records, line entries/rows/operations, and string length/work. The outer
object container is parsed by qBFD; the DWARF record limits do not limit
qBFD's own metadata work. Nothing here sandboxes object inspection.

Focused CTests (`ctest --test-dir <build> -R '^qdwarf_'`) use deterministic
ELF/PE/COFF/Mach-O fixtures, including big-endian DWARF64, lookup boundaries,
unsupported and malformed records, and resource limits.
