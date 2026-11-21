# Specification libraries, modules, and imports

LLSynth libraries reuse grammar rules, typed functions, states, IR dialects,
and other specification declarations. The generator links them before compiling the grammar, actions,
types, and IR. Generated Python and C/C++ tools remain self-contained: they do
not load `.llsynth` files, need the generator, or require a runtime module path.
An `.import` belongs to the specification, not the language it defines.

## Example

`words.llsynth`:

```text
.library "Words"
.summary "Number syntax and arithmetic"
.export Number, increment

.token NUMBER: Int = /[0-9]+/

.function increment(x: Int) -> Int
return x + 1;
.end

.rule Number
'syntax: n:NUMBER
'semantics: return $n;
.end
```

`main.llsynth`:

```text
.language "Example"
.import "words.llsynth" as W

.rule Start
'syntax: n:W__Number EOF
'semantics: return &W__increment($n);
.end
```

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl \
    --mode=interpret -o example.py main.llsynth
printf '41' | python3 example.py
# 42
```

Both `W__Number` and `W.Number` refer to the same exported rule. The linker
resolves qualification before the grammar and action lexers run.

## Choosing an import form

For a library declared `.library "Library.Words"`:

| Import | How to call its exported function |
| --- | --- |
| `.import "Library.Words"` | `increment(x)` (also `Words__increment(x)`) |
| `.import qualified "Library.Words"` | `Library__Words__increment(x)` or `Library.Words.increment(x)` |
| `.import "Library.Words" as W` | `W__increment(x)` or `W.increment(x)` |

`qualified` may also follow the name. The `as` form uses only the chosen
alias. Ambiguous plain imports and collisions with local declarations are
errors; qualify one import to distinguish the names. Importing a legacy
`.module` without `as` retains the existing last-component alias binding only.
For ready-to-use libraries see the [stdlib reference](stdlib.md).

## Identity, search paths, and import-once

* An imported file must begin with `.library "Name"` or `.module Name`; blank
  lines and comments may precede it. Names are identifier components optionally separated by dots,
  such as `Library.Words`. A module name identifies exactly one canonical file
  within a generator invocation. Loading different files declaring the same
  name is an error, even if their contents match.
* `.import "relative/path.llsynth" [as Alias]` resolves a literal filesystem
  path. Absolute paths are also accepted. A quoted identifier such as `"Numbers"`
  or `"Library.Words"` first tries the literal path, then named lookup. Unquoted
  identifiers always use named lookup.
* `.import Library.Words [as Alias]` searches for `Library/Words.llsynth`.
  Its declared name must be `Library.Words`.
* Search order is the directory of the file **containing the import**, followed
  by each repeatable `--module-path DIR`, in command-line order. The first
  existing regular file wins. Paths in included fragments are relative to the
  fragment. `--include-path` is deliberately separate from `--module-path`.
  For `std.Foo` the final fallback is `stdlib/Foo.llsynth` beside the generator's
  `scripts/` directory. Copy that directory when installing the generator.
* The default alias is the final module-name component (`Words` above).
  An explicit alias is one identifier and cannot contain `__`. One alias may
  be repeated for the same module, but cannot denote different modules.
* File identity uses canonical paths, including symlink resolution. Repeated
  imports, path aliases, and diamond dependencies emit a module only once.
  Different aliases refer to the **same** declarations and state, not instances.
* Loading follows source-order, depth-first traversal. A module's declarations
  appear at its first import site; later imports only bind aliases. `.init`
  blocks are linked once and run once per normal generated-tool execution,
  not during generation. State and initialization ordering otherwise retains
  the existing LLSynth rules.
* Cycles are errors, not partially initialized modules. Diagnostics identify the
  closing import's file/line and the active file chain. Missing files report
  the import site and searched paths.

For example:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --check --mode=interpret \
    --module-path ./project-modules --module-path ./shared-modules main.llsynth
```

All transitively imported files and textual includes participate in the existing
SHA256 dependency manifest, Make/Ninja depfile, and input-overwrite protection.
Dependency paths are sorted; generated symbol identities depend on module
names, not absolute checkout paths. No directory enumeration or hash iteration
determines import order.

## Visibility and exports

Libraries export all local named declarations when no `.export` is present.
Adding an `.export` makes only the listed declarations public. Legacy
`.module` declarations are private by default. `.export` accepts a
comma-separated list of local declarations, and may precede their definitions:

```text
.export Number, increment, @counter, #symbols, !Failure, %Value, Stack
```

Ordinary identifiers export rules, tokens/literals, functions, or IR dialects.
Sigils identify states (`@`), symbol tables (`#`), exceptions (`!`), and type
aliases (`%`). Exporting an IR dialect exposes its operation constructors and
`Op` type. If different ordinary declaration categories share a spelling, an
ordinary export exposes each matching declaration. Unknown and duplicate exports
are errors. Plain imports do not implicitly re-export a dependency's names.

Access through alias `M` looks like:

| Declaration | Reference |
| --- | --- |
| Rule/token | `n:M__Number` in a syntax facet |
| Function | `&M__increment(x)` or `M__increment(x)` |
| State | `@M__counter` |
| Symbol table | `#M__symbols["key"]` |
| Exception | `Throw(!M__Failure, "reason")` |
| Type alias | `%M__Value` |
| IR operation/type | `M__Stack.PUSH(x)`, `M__Stack.Op` |

A module uses its own declarations without qualification. Dependencies require
explicit imports inside that module; importer aliases are not inherited.
Private names can repeat in different modules. Capture labels, `$captures`,
function arguments, strings, regexes, exception messages, and backend templates
are not blindly renamed. `Alias__...` is reserved in the importing declaration
and local-variable scope. To avoid ambiguity between bare function references
and local variables, callable declaration names cannot also be module parameter
or local-variable names. Use distinct names for callbacks and their declarations.
The same restriction applies to entry-file functions when modules are imported.

The linker emits internal `LLSMod_...` names. That prefix is reserved in source;
internal names shown in table dumps or downstream diagnostics are not a public
API. The namespace is a specification binding mechanism, not an untrusted-code
security sandbox. All participating modules are trusted language-specification
inputs.

## Entry configuration, lexer behavior, and includes

An optional `.summary "text"` describes the library and does not replace the
importer's summary. `.language`, `.version`, `.start`, `.option`, and `.pipeline` belong
to the entry specification and are rejected in imported libraries and modules. An entry file
may itself start with `.module Name` and contain exports when it is also intended
to be reusable. Running a reusable specification as the entry file still requires
the usual EOF-terminated entry rule and mode-appropriate facets.

Without an explicit `.start`, the default is the first rule belonging to the
entry specification (including its textual includes), **not** an imported rule.
An explicit `.start M__Start` or `.start "M__Start"` may select an exported rule.

Lexing remains one combined language lexer. Importing tokens and `.skip` rules
contributes lexical behavior even when those declarations are private; export
controls name visibility, not token activation. Existing longest-match and
source-line priority rules still apply. Avoid competing token regexes in module
libraries: modules do not create lexer modes or separate parsers.

`.include "file"` remains textual, relative-first, and repeatable. It uses
`--include-path`, preserves original file/line locations, and retains its existing
cycle diagnostics. Included declarations belong to the including module's scope.
Including the same declaration twice is still a duplicate-declaration error;
import-once does not change include behavior. Use `.import`, not `.include`, for
files containing `.module` or `.library`.

## Generator-time companions

Use `.extern "exports.pm"` in a `.library` or `.language` specification to
contribute declarations from a Perl file. The file runs once per importing
unit, **including during `--check`**. It can return a scalar
of LLSynth declaration text or an array reference of declaration lines. For
example, `exports.pm` could end with:

```perl
[
    '.function answer() -> Int',
    'return 42;',
    '.end',
]
```

An importing specification can call `answer()` just like a handwritten
function, subject to the library's `.export` and import form. Paths are
relative to the file containing `.extern`. Imported declarations can use the
scope's names; place `.import`, `.include`, and further companions in the
specification itself. The dependency manifest, depfile and output-overwrite
checks cover companion files. Only load trusted companions: Perl code or
native code runs in the generator process.

A Perl companion can alternatively return a hash mapping new block directive
names (without the dot) to expansion callbacks. A callback receives the trimmed
header, the body as `[text, file, line]` triples, the header file and line,
and a callback that reads a file's bytes and records it as a build dependency.
It returns LLSynth declaration text in the *using* scope. The reader consumes
the closing `.end`, so the body excludes it. A direct import makes the library's
blocks available from that import onward, even when qualified. See
[std.ISel](isel.md) for a complete provider.

For a C companion, use `.extern "exports.c"` and implement this ABI:

```c
const char *llsynth_declarations(void) {
    return ".function answer() -> Int\nreturn 42;\n.end\n";
}
```

The generator compiles `.c` files with Perl's `ExtUtils::CBuilder` in a
temporary directory and loads the resulting shared library. This needs a
host C compiler; no build outputs are placed beside the source.
For a prebuilt provider, `cc -shared -fPIC exports.c -o exports.so` on Linux
produces a companion usable with `.extern "exports.so"`. Both forms use the optional
Perl `FFI::Platypus` module (API 2) to call the provider. Return a non-null
NUL-terminated UTF-8 string in storage that remains valid after the call.
`.pm` and `.pl` select Perl by default; use `.extern perl "path"` or
`.extern c "path"` to select the provider format explicitly. Generated tools
need neither the provider nor Perl FFI. For a function **called at runtime**,
see [`.extern c Name(...) -> Type from "..."`](interop.md).

## Implementation and extension boundary

`scripts/LLSynthModules.pm` contains the loader and symbol linker.
`parse_spec` calls `read_module_spec_lines($file, \%dependencies)`; the existing
`read_spec_lines` remains the textual-include reader. The new reader returns the
same `[text, source_file, line]` records, so the remaining compiler and emitters
do not need a module-aware runtime or a separate bytecode format.

The linker handles existing rule facets, function/init actions, state
initializers, type references, IR declarations/constructors, rewrite IR headers
and guards, and backend IR headers. For the independently implemented language
extensions, it also links:

* `.async function Name(...) -> Type` and its action body.
* `.extern c Name(...) -> Type from "library" symbol "symbol"`: only the local
  callable name and specification type references are linked. The library and
  native ABI symbol strings are unchanged.
* `.sandstone Name bank "..." syntax "..." cli "..."`: only the local callable
  name is linked. Bank, syntax, and executable strings are unchanged.
* Single-line `.template Name(x) = expression`, including calls in its
  expression, and `.macro Name(x)` / `.end`, including its action body. Exported
  metaprograms use ordinary qualified calls such as `M__double(x)`. Their names
  and parameters follow the same no-shadowing rule as callable declarations.

The loader first links source records. The action compiler then parses those
records and expands metaprogram ASTs before action/type validation. This preserves
module definition-site references without combining unlinked macro dictionaries.
The linker-level regression test `tests/modules/extensions.t` verifies textual
binding and opaque foreign strings; `tests/integration/test_features.py` verifies
combined macro, closure, async, native-import, and module behavior.
Grammar-producing macros and rewrite replacement expressions are not generic
action-language blocks and are not blindly rewritten. Rewrite replacements
should use their pattern-bound operands, not imported helper calls.

Install `LLSynthModules.pm` and `LLSynthCompanions.pm` beside `llsynth.pl`
and its other sibling helpers. Keep the bundled `stdlib/` beside `scripts/`.

Run the module regression suite with:

```sh
python3 cornerstone/LLSynth/tests/modules/test_modules.py
python3 cornerstone/LLSynth/tests/modules/test_libraries.py
```
