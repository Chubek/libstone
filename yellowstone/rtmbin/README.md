# rtmbin — offline runtime sample attribution

`rtmbin` uses qBFD to read executable/shared-image metadata and attribute
caller-supplied program counters to file-backed executable byte ranges. It
does not collect samples, open a running process, disassemble code, infer
instruction semantics, or modify an image. The verification-plan command
checks caller-selected byte spans; it does not pick instrumentation sites
or generate instrumentation. The separate graph command uses Wetstone on
an explicit model, never on a graph inferred from an image.

## Build and use

```sh
cmake -S yellowstone -B build-yellowstone \
  -DYELLOWSTONE_BUILD_TOOLS=ON -DYELLOWSTONE_BUILD_TESTS=ON -DQOBJFILE_DOMTERM=OFF
cmake --build build-yellowstone --target rtmbin_cli rtmbin_tests
ctest --test-dir build-yellowstone -R '^rtmbin_' --output-on-failure
```

The C++20 static library is always built and exposes `rtmbin::rtmbin`.
For installed use, call `find_package(Yellowstone CONFIG REQUIRED)` and link
`rtmbin::rtmbin`. The public header is `<rtmbin/rtmbin.hpp>`. qBFD and the
vendor-independent Wetstone graph headers are private implementation details
of this target; no Wetstone solver or separate Wetstone package is needed.

```cpp
#include <rtmbin/rtmbin.hpp>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    auto result = rtmbin::Image::from_file(argv[1]);
    if (const auto* error = std::get_if<rtmbin::Error>(&result)) {
        std::cerr << error->message << '\n';
        return 1;
    }
    const auto& image = std::get<rtmbin::Image>(result);
    std::cout << rtmbin::name(image.format()) << ": " << image.ranges().size() << " ranges\n";
}
```

`Image::from_bytes` also accepts a byte span. Both factories retain only
normalized metadata; the original bytes are not kept. `Image::attribute`,
`Image::profile` and `Image::verification_plan` can then share one `Image`.

## Range and address semantics

| Input | Ranges | Preferred base |
| --- | --- | --- |
| ELF32/64 executable or shared object (including PIE) | `PT_LOAD`, executable if `PF_X` is set | Lowest `PT_LOAD` virtual address |
| PE32/PE32+ executable or DLL | Section virtual spans, executable if `IMAGE_SCN_MEM_EXECUTE` is set. File-backed size excludes raw alignment padding beyond a nonzero `VirtualSize`, and BSS is not file-backed. | Optional header `ImageBase` |
| Thin Mach-O executable or shared image recognized by qBFD | Segment virtual spans with initial execute protection | Lowest file-backed segment virtual address |

Ranges are half-open `(start, length)` intervals. The declared executable
file bytes are the prefix of each executable virtual range; zero-fill tails
do not count. `layout` shows non-executable ranges too, in linked-address
order. PE headers, gaps, and undeclared page-alignment padding do not become
range entries. ELF and Mach-O executable segments can contain mapped file
headers or other non-instruction bytes; a match does **not** prove that a byte
is code, is an instruction boundary, or was executed. Permissions reflect
image metadata, not a live process's page permissions.

By default PCs are **linked virtual addresses**, not file offsets or RVAs.
With `--runtime-base BASE`, pass the observed runtime address corresponding to
the reported `preferred_base`. The conversion is
`linked = preferred_base + (PC - BASE)`. It is checked against the image's
address width, including `PC >= BASE`. This models one image with a uniform
slide; it does not discover ASLR, associate modules with addresses, or
normalize pointer tags. The caller is responsible for using the matching
image and correct runtime base.

```sh
rtmbin layout ./app
rtmbin attribute ./app 0x401003
rtmbin profile ./app ./samples.txt --runtime-base 0x7f0000000000
rtmbin plan ./app ./points.txt --runtime-base 0x7f0000000000
```

`attribute` always classifies a well-formed PC query, including misses:
`outside_ranges`, `non_executable`, `not_file_backed` (an executable range's
zero-fill tail), or `address_out_of_range` (conversion/width check failure).
Only `attributed` includes a file offset.

Sample records are `PC [WEIGHT]`, with positive weight defaulting to 1.
`profile` returns the total weight, one mutually exclusive status bucket
for each sample, and attributed weight by range. Invalid weights or total
overflow fail the report without partial output.

Plan records are `NAME PC [SIZE]`, with positive size defaulting to one byte.
Names must be unique and 1–64 ASCII letters, digits, `.`, `_` or `-`.
For example, if the layout has a file-backed executable span at runtime
address `0x7f0000001000`:

```text
entry.observation 0x7f0000001000 4
```

`plan` reports the named spans in input order with linked addresses and file
offsets. Every byte must be in the same executable file-backed range as the
span's first byte. Names label observations, not inferred routines. A
successful plan attests only to range geometry in the input snapshot, not
file identity, patchability, instruction boundaries, or semantics.

## Explicit model summary

```sh
rtmbin graph ./model.txt
```

```text
entry 1
block 1 2 3     # two successor IDs; neither edge has a predicate
block 2 2      # reachable self-loop
block 3        # no successors
block 4        # unreachable
```

Wetstone reports sorted reachable/unreachable block IDs and sorted reachable
cyclic strongly connected components, including self-loops. This is
structural reachability on user-supplied edges, not path feasibility,
symbolic evaluation, or proof of behavior. IDs are abstract labels (not
PCs); they must be distinct 32-bit unsigned values below `UINT32_MAX`. A
block can have zero, one, or two distinct existing successors.

## Limits and scope

- Image inputs are capped at 256 MiB; file inputs must be regular files.
  qBFD parse failures propagate as diagnostics. Archives, relocatable
  objects, core dumps, universal Mach-O containers (provide a thin slice),
  Mach-O `SG_HIGHVM` mappings, and overlapping declared virtual ranges are
  unsupported. This is an offline reader, not a sandbox for hostile input.
- CLI text files are capped at 16 MiB and 4096 bytes per line; numbers are
  unsigned decimal or `0x` hex. Blank lines and `#` comments are allowed.
  CLI profiles accept at most 100,000 records. Plans accept at most 10,000
  points and models at most 1024 blocks (both CLI and library).
- CLI exits are `0` for a report (including an attribution miss), `1` for
  an input or report error, and `2` for usage errors.
- No transformation, live instrumentation, code execution, symbolic
  execution, automated collection, or semantic claims about native targets
  are implemented. The focused regression fixtures are host-independent and
  do not run the target code.
