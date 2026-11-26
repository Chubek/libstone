# ASDL source grammar

This grammar is implemented by the native reader. It preserves the legacy
Perl reader's grammar and adds the dot directives and `::=` definitions used
by [`Mini`](examples/Mini.asdl), [`MiniML`](examples/MiniML.asdl) and
[`MiniRuby`](examples/MiniRuby.asdl).

## Definitions and files

```ebnf
file        = [c-prologue] {%option-line} ["%%"] declarations ["%%" c-epilogue] ;
declarations = {declaration} | "module" ident "{" {declaration} "}" ;
declaration = definition | directive ;
definition  = type-id "=" rhs [";"] | type-id "::=" rhs ";" ;
rhs         = fields | ctor {"|" ctor} ["attributes" fields] ;
ctor        = ctor-id [fields] ;
fields      = "(" field {"," field} ")" ;
field       = field-type ["<" refinement ">"] ["?" | "*" | "+"] [ident] ;
field-type  = type-reference | "list" "[" field-type "]"
            | "map" "[" field-type "," field-type "]"
            | "array" "[" [field-type ","] positive-decimal "]" ;
type-reference = {ident "."} type-id ;
refinement  = ":" ident | dotted-name "!" ;
dotted-name = ident {"." ident} ;
```

Identifiers start with an ASCII letter and continue with letters, digits
or underscores. Definition names and the last component of type references
start in lowercase, and constructor names start in uppercase. Qualified
names are contiguous. Each file requires at least one definition. Parentheses
in a field list are nonempty; write `Empty`, not `Empty()`. Named types are
visible throughout their file, including in earlier definitions. The builtin
types and container syntax are the same as in the Perl reader. The legacy
`=` syntax retains Zephyr's restriction on recursive products; `::=`
products can participate in recursive types, as in MiniML and MiniRuby.

Between tokens, whitespace and `#`, `--`, `//`, or `/* ... */` comments are
ignored. Only after `.modular` is `#true` or `#false` a token. Strings are
single- or double-quoted; backslash quotes the following character, with
`\n`, `\r`, and `\t` denoting the corresponding control characters.

### Legacy envelope

A bare file or `module` wrapper needs no separators. A `%{ ... %}` C prologue,
if present, comes first. The following option lines use `%name [value]` or
`%option name[=value] ...` and require a first `%%`, as does a prologue. The
second `%%` starts an opaque C epilogue that extends through EOF. Each `%%`
occupies a line on its own. The C prologue's `%}` is recognized outside C
quoted literals and comments. The closing marker of the prologue is not
part of the fragment.

Option values can be quoted; within quotes, backslash quotes the next
character. At the start of a word, `#`, `--`, and `//` begin a comment.
Single options are `prefix`, `guard`, `header`, `source`, `visitor`,
`listener`, `include`, `schema`, `builtins-header`, `omit-builtin`
(or `no-builtin`), and the booleans `enums`, `line-directives`,
`header-only`, and `builtins`. Boolean values include `on/off`, `yes/no`,
`true/false`, and `1/0`; an omitted value means true. `%option` accepts
the `no-` spelling of boolean options without a value. Later values
replace earlier values, except `omit-builtin` accumulates values.

## Extended directives

```ebnf
directive   = ".language" string ";" | ".summary" string ";"
            | ".version" string ";" | ".unit" ":" type-reference ";"
            | ".modular" ("#true" | "#false") ";"
            | ".import" ["schema"] import ";"
            | ".logistics" "for" ":" type-reference "::=" "lua"
                "(" lua-id {"," lua-id} ")" ["->" "(" [lua-id {"," lua-id}] ")"]
                lua-source ".end" [";"] ;
import      = dotted-name ["as" ident] | selection {"," selection} "from" dotted-name ;
selection   = import-name ["as" import-name] ;
import-name = ident ["!"] ;
```

Language, summary, version, unit, and modularity are optional and may each
appear at most once. `.unit` names a visible ASDL type, not a builtin. A nominal
refinement (`identifier<:variable>`) is descriptive metadata; it does not
replace the base type. An executable refinement (`expression<foldable!>` or
`expression<Rewrite.foldable!>`) names a selective or namespaced Lua binding.
The resolver checks the binding is visible; the runtime loads and checks the
Lua export. A selective alias for a predicate must retain its trailing `!`.

### Lua imports versus schema imports

```asdl
.import CheckCoverage, foldable! as ready! from std.rules;
.import std.rewrite as Rewrite;
.import schema ast.shared as Shared;
.import schema token as local_token from ast.lexical;
file ::= (Shared.node root, local_token* tokens);
```

An import without `schema` describes a Lua library. Selective imports bind
only the requested names; namespace imports bind their alias or the final
component of the library path. The grammar does not load or run Lua
libraries. The set of Lua import bindings is scoped to its source file.

A schema import locates `a/b.asdl` for `a.b`, first relative to the importing
file and then in the explicit search roots, in order. An imported schema
must declare `.modular #true;`. The root is permitted to import modules
regardless of its modularity. Imports are canonicalized by filesystem path;
an active path that is visited again is an error with the cycle trace.
Repeated references to the same file in an acyclic graph share one set of
definitions. The same dotted module name may not resolve to different files
in one graph.

An unaliased schema namespace import provides its full path and final
component as qualifiers; an aliased import provides only the alias.
Selective schema imports bind unqualified type names and may use lowercase
aliases. Qualified and selected names are visible only in the importing
file; imports do not re-export other imports. Imported declarations,
including their callbacks, are flattened into the resolved model with
module-mangled internal names and original source locations. The root's
legacy envelope, module name, options and dot metadata control the combined
output; imported prologues and epilogues are not copied.

### Logistics signatures and source spans

Each type can have one hook. Its first parameter receives the node;
remaining parameters declare inherited arguments, by position. An optional
same-line `-> (result, ...)` names synthesized results. It does not change
the Lua body or turn parameter/result names into types. Parameter and result
names follow Lua identifier syntax and cannot repeat within one list.

The body starts immediately after the signature and ends at `.end` on its
own line, with optional horizontal indentation, optional `;`, and optional
trailing line comment. Only `.end` in Lua's normal lexical state ends the
body; occurrences in short strings, escaped line continuations, line comments,
long strings or long comments are not delimiters. Long brackets match exactly
the opening number of equals signs. Body bytes and source spans are retained
verbatim; Lua syntax beyond the lexical boundary is checked by the runtime,
not the ASDL reader.

Diagnostic positions use one-based lines and byte columns. Source spans have
half-open byte offsets. `asdl::parse` only parses; `asdl::resolve` binds
schema references and checks import/hook metadata; `asdl::parse_file` performs
both. The shared C++ model's `asdl::validate` checks declaration invariants
and assigns generated names and indices. The resolver does not verify the
behavior of executable refinements or execute logistics.
