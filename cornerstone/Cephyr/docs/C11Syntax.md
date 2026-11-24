# C11 syntax documents

`--emit-ast=<format>` preprocesses and parses one C translation unit and
writes its AST. It stops before lowering, target code generation, or MIR
hooks. It uses Cephyr's existing [C11 frontend](../C11.md): parse-only may
accept constructs that the code generator cannot lower, but does not add
support for syntax the frontend itself rejects.

```sh
cephyr --emit-ast=json example.c -o example.json
cephyr --emit-ast=yaml example.c -o example.yaml
cephyr --emit-ast=sexp example.c -o example.sexp
cephyr -c example.json -o example.o
cephyr -S -x c11-ast-yaml - -o - < example.yaml
```

AST output defaults to stdout (or the result buffer when embedding). Source
input can be a `.c` or preprocessed `.i` file, or `-` for stdin. For syntax
input use a `.json`, `.yaml` / `.yml`, or `.sexp` / `.sexpr` / `.sxp` filename,
or specify `-x c11-ast-json`, `-x c11-ast-yaml`, or `-x c11-ast-sexp`.
You can compile a syntax document or use `--emit-ast` to convert it to
another syntax format.

## Version 1

All three formats encode the same data model from
[`C11Syntax.schema.json`](../contracts/C11Syntax.schema.json).
JSON and YAML output put `schema_version: 1` first in the outer object.
S-expression output starts with `(1 ` and then encodes the other fields
with `(object ("key" value) ...)` for maps and `(array value ...)` for
arrays. Empty containers are written as `(object)` or `(array)`. Strings
use JSON quoting/escapes, and scalars also include signed 64-bit decimal
integers, `true`, `false`, and `null`. YAML uses JSON-compatible scalar
types; anchors, aliases, explicit tags, and multiple documents are not
supported. Duplicate keys are rejected in all formats.

- `ast` is the parser tree. Nodes have a `kind`, named `children` (an absent
  child is `null`), ordered `items` for list nodes, and `attributes` for
  literal or declarator data. When present, `line` is one-based in the
  preprocessed text. Integer literal values use decimal strings to preserve
  all 64 bits; real literals retain their spelling. String nodes retain
  numeric byte or wide code-unit arrays, including embedded zeros.
- `source` stores UTF-8 preprocessed text and its diagnostic locations (`file`
  and a sparse `line_map`). These file names are labels, not files to reopen.

Cephyr validates the decoded data against its embedded copy of the schema
using Valijson. It then replays the preprocessed text to rebuild parser
bindings, checks that the rebuilt AST matches the serialized AST, and only
then passes that tree to the normal compiler. Version 1 is a round-trip
format, not an AST-only editing interface: changing just the tree or just
the source may leave them inconsistent. Compilation does not repeat
preprocessing, so macro expansion, includes, and source hooks must already
have been applied when the document was created. Use compatible target and
C environment options for both invocations. Schema validation does not
guarantee that a translation unit can be compiled. Documents are limited
to 64 MiB and bounded nesting and value counts.

## SDK and embedding

Use `cephyr_parse_c11` from `<cephyr/Cephyr.h>` or `cephyr::parse_c11` from
`<cephyr/Cephyr.hpp>`. Both use the usual compiler request, with a separate
format argument (`CEPHYR_AST_JSON`, `CEPHYR_AST_YAML`, or `CEPHYR_AST_SEXP`).
The native SDK's source hooks are available via `cephyr_request.sdk`.
Python exposes `cephyr.parse_c11(["-"], source, format="json")` and the
same method on `Compiler`. To import, call the usual compilation function
with a path or the corresponding `-x c11-ast-… -` arguments and stdin data.
