# C11 declaration interfaces

`--emit-interface=sexp` reports the resolved file-scope declarations of one
C11 translation unit for the selected target. Use it when you need C types
and layouts rather than source spellings or generated assembly.

```sh
cephyr --target=x86_64-linux-gnu --emit-interface=sexp example.c
cephyr --target=aarch64-linux-gnu --emit-interface=sexp example.c -o example.interface
cephyr --emit-interface=sexp -x c - < example.c
```

Input can be raw C (`.c`), preprocessed C (`.i` or `-x cpp-output`), or a
validated [C11 syntax document](C11Syntax.md). Raw C is preprocessed normally,
including headers, macro expansion, and any configured source and directive
hooks. These steps are not repeated for preprocessed C or syntax documents.
Use compatible target and C environment options when saving a syntax document
and projecting from it.

Without `-o`, or with `-o -`, Cephyr writes the interface to stdout (the
result buffer when embedding). This is a separate compilation stage; do not
combine it with `-E`, `-fsyntax-only`, `--emit-ast`, `--emit-mir`, `-S`, or `-c`.
Invalid syntax or declaration constraints are reported with the usual Cephyr
diagnostics; use `--diagnostics-format=sarif` for structured diagnostics.

## Scope

The projection includes declarations contributed by headers and is not a list
of exported symbols from an object file. It reports resolved typedefs, tags,
objects, and function declarations, including C linkage and definition
information. Use the type and layout data for the same target you will
compile for. Version 1 supports the built-in SysV AMD64, AAPCS64, and RV64IM
C layouts, used by `x86_64-linux-gnu`, `aarch64-linux-gnu`, and
`riscv64-linux-gnu`, respectively. All three use LP64. SDK-only C target ABIs
are not supported. An SDK assembly override for a built-in target does not
change its C layout.

Cephyr parses the translation unit and resolves file-scope declarations, but
does not lower function bodies, run MIR hooks, or invoke the assembler or
linker. Success is not a guarantee that the entire translation unit can be
compiled. The result is not a compilable AST. It identifies the calling
convention but does not describe register assignments, argument passing,
machine code, or final linker visibility.

## Version 1

The result is a single UTF-8 S-expression with `interface` as its first atom
and `1` as its version. Names and triples are quoted strings with JSON
escaping. Integers are decimal, and `true`, `false`, `none`, and `unknown`
are unquoted atoms. For example, `extern int count;` on AMD64 produces:

```lisp
(interface 1
  (target "x86_64-linux-gnu")
  (data-model LP64)
  (abi "SysVAMD64")
  (object "count"
    (linkage external)
    (storage extern)
    (definition declaration)
    (layout (size 4) (align 4))
    (type (integer int (rank 3) (bits 32) (signed true) (size 4) (align 4)))))
```

Consumers should parse the S-expression, not match indentation or line breaks,
and check the version and target before interpreting the declarations.

- A `typedef`, `object`, or `function` form names an entity and gives its
  resolved `(type ...)`. Compatible redeclarations are combined into one
  entity with a composite type. Typedef uses are resolved to the underlying
  type; the typedef name is also reported as its own declaration.
- For objects and functions, `(linkage ...)` gives effective C linkage,
  while `(storage ...)` lists the written storage class (`static`, `extern`,
  or `none`) on each declaration in source order. `(definition ...)` is
  `declaration`, `tentative`, `defined`, or `inline-only`. Functions also report
  `calling-convention` (`sysv`, `aapcs64`, or `lp64`), `inline` and `noreturn`
  specifiers when present, and `parameter-names` in signature order (`none`
  for unnamed slots). Names come from the definition if available, otherwise
  from the first declaration with named parameters.
- Types include `void`, `integer`, `real`, `pointer`, `array`, `function-type`,
  `qualified`, and `tag-ref`. Integer types retain their C rank, width,
  and signedness, including target-specific plain `char` signedness. Real
  types report their representation (for example, `x87` or `binary128` for
  `long-double`) separately from their size in memory. Array types report
  their element type and resolved count, or `unknown` for an incomplete bound.
  An incomplete tentative array with external linkage is completed with one
  element at the end of the translation unit.
- Function types distinguish unprototyped `f()` from the zero-parameter
  prototype `f(void)` and indicate variadic prototypes. Parameters are
  projected as types, not declarator spellings: array and function parameters
  become pointers, and top-level parameter qualifiers are omitted from the
  signature. Qualifiers on a pointed-to type are retained. Array-parameter
  `static` minimum bounds are not part of the projected signature.
- `(tag ID KIND NAME ...)` describes a `struct`, `union`, or `enum`. An
  unnamed tag has `NAME` set to `none`. `(tag-ref ID)` refers to that entry;
  IDs are local to the document and allow recursive types. Incomplete tags
  are marked `(complete false)` without a layout. Complete enums show the
  underlying integer type and enumerator values.
- Layout sizes, alignments, field offsets, and bit-field container offsets
  are in bytes. Unknown size or alignment is written `unknown`. An object's
  effective alignment includes any `_Alignas` request. Complete records
  list their fields in declaration order. Bit-fields report a width in bits
  and a shift from the least significant bit at the field's byte offset,
  along with the storage container's byte offset within the record.
  A flexible array member is identified separately and has no fixed count.

## Embedding

Use `cephyr_emit_interface(&request, &result)`, `cephyr::emit_interface(args,
source)`, or `Compiler.emit_interface(args, source)`. These select the
interface stage and otherwise use the ordinary compiler arguments, source
buffers, result ownership, and diagnostic formats. See the
[embedding guide](../EMBEDDING.md#resolved-c11-declarations) for examples.
