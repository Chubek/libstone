# Inspecting ArchML profiles and code with `std.isa`

`std.isa` uses Sandstone's [ArchML](../../../sandstone/archml/README.md)
library to validate and query ISA profiles and decode flat machine code.
Import it as `.import std.isa as ISA`. The API takes runtime paths to
`.aml` or `.am` files and returns Strings:

| Call | Result |
| --- | --- |
| `ISA:profileName(profile)` | The profile name. |
| `ISA:metadata(profile)` | The complete validated ArchML document as JSON. |
| `ISA:record(profile, kind, name)` | One named ArchML record as JSON, or the String `null` if absent. |
| `ISA:analyze(profile, hexBytes)` | A JSON report for bytes in target order, including decoded instructions and declared effects. |

For example, save this as `isa-demo.llsynth` and run it from the repository
root:

```text
.language "ISA demo"
.import std.isa as ISA
.rule Start
'syntax: EOF
'semantics: return ISA:analyze("sandstone/archml/stdspec/riscv64.aml", "b3 00 31 00 83 30 01 01");
.end
```

```sh
cmake -S cornerstone/LLSynth/stdlib/isa -B build/llsynth-isa
cmake --build build/llsynth-isa --target llsynth_isa
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T python3 \
  -o isa-demo.py isa-demo.llsynth
LD_LIBRARY_PATH="$PWD/build/llsynth-isa${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  python3 isa-demo.py </dev/null
```

The report has `profile`, `byteLength`, and `instructions`. Each instruction
has a zero-based byte `offset`, `size` in bytes, concrete form `name`, and its
unexpanded `assembly` template. Operands are listed in declaration order
with `name`, `kind`, `access`, and `value`. `value` is a decimal String to
preserve the full unsigned 64-bit range; signed operands are sign-extended
and scaled by ArchML. Register values are encoding numbers, not register
names. `features` and `modes` list the form's declared requirements.
`effects`, `implicitUses`, and `implicitDefs` contain the items from the
corresponding ArchML property, or `null` when the property is absent.
In the example, `add` starts at offset 0 and `ld` starts at offset 4 with a
`memory` effect and a displacement of `"16"`.

Metadata and records use ArchML's JSON format: lists become arrays, quoted
strings are JSON strings, and unquoted symbols are `{"symbol":"spelling"}`.
For example, `ISA:record(path, "instruction", "ld")` returns the full `ld`
record, including its P-code, and `ISA:record(path, "target", "riscv64")`
returns the target's properties. Reports preserve that representation for
items of the effect and implicit-register properties.

## Runtime and limits

* Hex input is case-insensitive. Spaces, tabs, and line breaks are allowed
  between complete byte pairs; neither `0x` prefixes nor separators inside a
  byte are accepted. Empty input produces an empty report. The limit is
  64 KiB of decoded input bytes per call.
* Every call reads and validates the current profile, relative to the
  generated tool's working directory. The profile is not needed at generation
  time and is not embedded. Missing/invalid profiles, malformed hex, and
  unmatched (including truncated) encodings raise `!ISA->Analysis`. An
  unmatched encoding identifies its offset and no partial report is returned.
* This is a linear sweep of ArchML's supported forms. When encodings overlap,
  ArchML picks the first matching form in source order. The report exposes
  declared metadata, not inferred behavior; it does not filter by feature or
  mode, evaluate P-code, recover control flow, or prove that the bytes are
  executable. Text-only operations can be queried but cannot be decoded.
* The companion (`LLSynth::ISA`, output `libllsynth_isa.so`) uses an existing
  `Sandstone::ArchML` target if available, finds the installed `ArchML` package,
  or builds the in-tree ArchML library. No assembler or subprocess is used.
  Python loads the companion on first use; add its directory to the platform
  loader path. C99/C11 tools call the native bridge directly. For Linux, generate
  `isa-demo.c` with `-T c11` and compile with:

  ```sh
  cc -std=c11 isa-demo.c -L build/llsynth-isa \
    -Wl,-rpath,"$PWD/build/llsynth-isa" -lllsynth_isa -lm -o isa-demo
  ```

  Like other `.extern c` libraries, generated C++ tools are not supported.

Run the focused tests against a built companion:

```sh
LLSYNTH_ISA_LIBDIR="$PWD/build/llsynth-isa" \
  python3 cornerstone/LLSynth/tests/stdlib/test_isa.py
```
