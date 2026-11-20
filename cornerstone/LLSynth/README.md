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
..
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

* [C11 and Sandstone interoperability](docs/interop.md): typed C ABI
  imports and assembly-to-bytes through Sandstone's CLI (Python) or SDK (C).
* [Modules, loaders, and imports](docs/modules.md): private-by-default
  specification modules, explicit exports, import-once loading, and
  repeatable `--module-path` search roots.
* [Closures, async functions, and concurrency](docs/execution.md): lexical
  closures, lazy tasks, spawning, and joining with isolated task snapshots.
  These execution features currently require the Python tool target.
* [Templates and hygienic macros](docs/metaprogramming.md): expression
  templates and statement macros expand action ASTs before type checking.
  Macro arguments evaluate once; expression templates may duplicate evaluation.

## Generator layout

`scripts/llsynth.pl` is the driver in this directory. Keep
`scripts/LLSynthCompiler.pm`, `scripts/LLSynthEmit.pm`,
`scripts/LLSynthMeta.pm`, and `scripts/LLSynthModules.pm` beside it when moving
or installing the generator. It resolves those modules relative to the real
driver path, not the working directory. The repository-root
`scripts/llsynth.pl` is the compatibility entry point.

The driver owns CLI handling, diagnostics, output writing, and the embedded
Python and C runtime templates. The compiler module parses, compiles and
validates the specification; the emitter module builds the target artifacts.
The metaprogramming module expands templates and hygienic macros.
The modules helper loads and links imported specification declarations.
The modules are internal implementation files, not a public Perl API.

The generator uses Perl and its core modules; it doesn't invoke a compiler or
Python itself. Generated files do not need Perl or the generator's sibling
modules. Split C/C++ output does need its generated companion files.
Specifications opting into native imports or assembly also require their
declared libraries or Sandstone installation at build/runtime, as documented
above.

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
