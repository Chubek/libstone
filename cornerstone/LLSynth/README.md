# LLSynth

LLSynth reads a `.llsynth` language specification and generates a Python 3 or
C/C++ language tool. The specification can describe lexical rules, syntax,
semantic actions, and stack IR lowering. The generator can also validate a
specification without emitting a tool.

## Usage

Run these commands from the repository root:

```sh
cornerstone/LLSynth/scripts/llsynth.pl --help
cornerstone/LLSynth/scripts/llsynth.pl --check --mode=interpret tiny.llsynth
cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T python3 -o tiny.py tiny.llsynth
printf '42\n' | python3 tiny.py
```

For those examples, save this as `tiny.llsynth`:

```text
.language "Tiny"
.skip /[ \t\n]+/
.token NUMBER: Int = /[0-9]+/
.rule Start
'syntax: value:NUMBER EOF
'semantics: return $value;
.end
```

The last command prints `42`. To generate and build a C version instead:

```sh
cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T c99 -o tiny.c tiny.llsynth
cc -std=c99 tiny.c -lm -o tiny
printf '42\n' | ./tiny
```

`-T` selects the language of the generated tool, while `--program-target`
selects the default output language *when that tool compiles programs*. The
default generator mode is `both`, which requires the start rule to define both
`'semantics` and `'lower` facets. Use `--mode=interpret` for a specification
with only a semantics facet, as above.

## Language facilities

Close every specification declaration block with `.end` on its own line.
Rule facets run up to the next facet or the end of the rule. Existing rules
using `..` to close the rule or `'end` to finish a facet are still accepted.

* [C11 and Sandstone interoperability](docs/interop.md): typed C ABI
  imports and assembly-to-bytes through Sandstone's CLI (Python) or SDK (C).
* [Libraries, companions, and imports](docs/modules.md): declare a `.library`
  in LLSynth, use `.extern` to load Perl or C companions, and import exports
  directly, qualified, or under an alias. Legacy `.module` files remain
  private by default.
* [Types and aliases](docs/types.md): product fields, sum variants, curried
  callables, and symbol aliases. The [Double example](examples/typed/double.llsynth)
  uses a [typed library](examples/typed/Numbers.llsynth) to distinguish a
  parsed number from empty input.
* [Generator extensions](docs/extensions.md): emit Vim support files and
  typeset specifications with Perl extensions.
* [Bundled standard libraries](docs/stdlib.md): import `std.Functional`,
  `std.Collections`, `std.Parsec`, `std.SExpr`, and other libraries from your
  specification. Optional libraries expose host IPC, libshell, Equinox NG,
  Sandstone assembly and ISA descriptions, and Wetstone data-flow analysis.
* [Instruction selection](docs/isel.md): import `std.ISel` to select Sandstone
  ArchML instruction forms and emit `.insn` assembly for a legalized IR.
* [Embedded VMs](docs/vmgen.md): opt in to a VMWeave program target with
  `.vmgen for IR`.
* [ActionSemantics](docs/action-semantics.md): implement a rule facet with
  NovoStone action notation using `'semantics actionsemantics`.
* [ActionSemantics facets](docs/action-semantics.md): opt in to NovoStone's
  executable action notation for a rule's `'semantics` facet.
* [Tables, closures, and concurrency](docs/execution.md): hybrid tables,
  dynamic and lexical method calls, and lazy tasks with isolated snapshots.
  These execution features currently require the Python tool target.
* [Templates and hygienic macros](docs/metaprogramming.md): expression
  templates and statement macros expand action ASTs before type checking.
  Macro arguments evaluate once; expression templates may duplicate evaluation.

## Generator layout

`scripts/llsynth.pl` is the driver in this directory. When moving or installing
it, copy the entire `scripts/` directory and its sibling `stdlib/` and
`extensions/` directories. The driver resolves its modules relative to its real
path. The repository-root `scripts/llsynth.pl` is the compatibility entry point.

The driver owns CLI handling, diagnostics, output writing, and the embedded
Python and C runtime templates. The compiler module parses, compiles and
validates the specification; the emitter module builds the target artifacts.
The metaprogramming module expands templates and hygienic macros.
The modules helper loads and links imported specification declarations.
The modules are internal implementation files, not a public Perl API.

Pure LLSynth specifications require only Perl and core modules for generation.
See [library companions](docs/modules.md) for the additional dependencies when
loading C through Perl FFI. Generated files do not need the generator's Perl
modules. Split C/C++ output does need its generated companion files.
Specifications opting into native imports or assembly also require their
declared libraries or Sandstone installation at build/runtime, as documented
above. See the [native standard-library build](docs/stdlib.md#native-build)
for the bundled bridges.
Set [`LLSYNTH_LIBRARY_PATH`](docs/modules.md#identity-search-paths-and-import-once)
to a colon-separated list of directories to search before the default import
locations.

## Tests

Run all regression suites:

```sh
python3 cornerstone/LLSynth/tests/run.py
```

Or run the original suites individually:

```sh
python3 cornerstone/LLSynth/tests/cli.py
prove -r cornerstone/LLSynth/tests
python3 cornerstone/LLSynth/tests/emit/test_emit.py
```

The suites check the CLI, diagnostics, includes, grammar tables, manifests
and dependency files. Backend tests compare generated Python, C99 and C++17
tools and exercise the split C library API when native compilers are available.
