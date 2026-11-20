# Specification modules and loaders

LLSynth modules reuse **language specifications**, not executable host source.
The generator loads and links modules before compiling the grammar, actions,
types, and IR. Generated Python and C/C++ tools remain self-contained: they do
not load `.llsynth` files, need the generator, or require a runtime module path.
There is no dynamic Python/Perl/C import, native plugin loader, network loader,
or program-language `import` statement implied by this facility.

## Example

`words.llsynth`:

```text
.module Words
.export Number, increment

.token NUMBER: Int = /[0-9]+/

.function increment(x: Int) -> Int
return x + 1;
.end

.rule Number
'syntax: n:NUMBER
'semantics: return $n;
..
```

`main.llsynth`:

```text
.language "Example"
.import "words.llsynth" as W

.rule Start
'syntax: n:W__Number EOF
'semantics: return &W__increment($n);
..
```

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl \
    --mode=interpret -o example.py main.llsynth
printf '41' | python3 example.py
# 42
```

The alias separator is **two underscores**, not a dot. This spelling works in
the existing grammar and action lexers without conflating module qualification
with IR operation qualification.

## Identity, search paths, and import-once

* An imported file must begin with `.module Name`; blank lines and `#` comments
  may precede it. Names are identifier components optionally separated by dots,
  such as `Library.Words`. A module name identifies exactly one canonical file
  within a generator invocation. Loading different files declaring the same
  name is an error, even if their contents match.
* `.import "relative/path.llsynth" [as Alias]` resolves a literal filesystem
  path. Absolute paths are also accepted.
* `.import Library.Words [as Alias]` searches for `Library/Words.llsynth`.
  Its declaration must be `.module Library.Words`.
* Search order is the directory of the file **containing the import**, followed
  by each repeatable `--module-path DIR`, in command-line order. The first
  existing regular file wins. Paths in included fragments are relative to the
  fragment. `--include-path` is deliberately separate from `--module-path`.
  There are no implicit environment, home-directory, package-manager, network,
  or installed-system search roots.
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

Declarations in an imported module are private by default. `.export` accepts a
comma-separated list of local declarations, and may precede their definitions:

```text
.export Number, increment, @counter, #symbols, !Failure, %Value, Stack
```

Ordinary identifiers export rules, tokens/literals, functions, or IR dialects.
Sigils identify states (`@`), symbol tables (`#`), exceptions (`!`), and type
aliases (`%`). Exporting an IR dialect exposes its operation constructors and
`Op` type. If different ordinary declaration categories share a spelling, an
ordinary export exposes each matching declaration. Unknown and duplicate exports
are errors. There are no wildcard exports or implicit/transitive re-exports.

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

`.language`, `.version`, `.summary`, `.start`, `.option`, and `.pipeline` belong
to the entry specification and are rejected in imported modules. An entry file
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
files containing `.module`.

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

Install the new helper beside `llsynth.pl`, `LLSynthCompiler.pm`, and
`LLSynthEmit.pm`, and `LLSynthMeta.pm`. Like those helpers it is only a generator dependency, never a
generated-tool dependency.

Run the module regression suite with:

```sh
python3 cornerstone/LLSynth/tests/modules/test_modules.py
```
