# Translate MiniCalc into ANF

`Lang.g` defines the syntax, `Lang.asdl` defines the AST and its translation
mapping, and `MySource.lang` is a sample program. From `cornerstone/Archon`:

```sh
./tools/archonc stdspec/riscv/rv32i.arch -o /tmp/translate-target.archdb
./tools/archon-irgen anf /tmp/translate-target.archdb --no-oracle -o /tmp/translate-anf
cd examples/translate
python3 /tmp/translate-anf/Translate.py --grammar Lang.g --absyn Lang.asdl \
    --out /tmp/MyProgram.i MySource.lang
cc -std=c99 -Wall -Wextra -Werror -pedantic \
    '-DARCHON_IR_PROGRAM_FILE="/tmp/MyProgram.i"' -I/tmp/translate-anf \
    /tmp/translate-anf/IR.c decode.c -o /tmp/decode-minicalc
/tmp/decode-minicalc
```

The script needs Python 3.10+ and Perl 5.14+ with core modules. It needs no
checkout or C compiler; the C command above separately exercises the
generated IR's decoder and verifier. The sample emits valid ANF with several
functions, not assembly or an executable: the legacy ANF pipeline has stricter
limits than the ANF representation.

## Adapting the sample

The tool does not infer semantics from a grammar or the names of its AST
constructors. A version 1 JSON mapping is required. Put it in leading
`-- archon-translate-begin` / `-- archon-translate-end` lines in the ASDL
file, with each line of JSON prefixed by `-- `, as in the sample. These are
ordinary ASDL comments. For a separate mapping, omit the inline block and
provide `<grammar-stem>.translate.json` beside the grammar. `--mapping FILE`
explicitly selects a JSON file instead of either convention.

- `integer_type` selects the ANF type for integer literals: `i8`, `i16`,
  `i32`, `i64`, `u8`, `u16`, `u32`, or `u64`. `productions` has an entry
  for every grammar production, with `type` (its ASDL result) and
  `alternatives` in written order. Each alternative specifies `rhs` (the
  exact sequence of grammar symbols including terminal quotes/delimiters;
  use `[]` for `%empty`) and `build`. `%dprec` is not part of `rhs`.
- `build` is one of `{"take": N}`, `{"node": "Tag", "fields": {"field": N}}`,
  `{"list": [N, ...]}`, or `{"cons": [Head, Tail]}`. Positions are one-based
  indices into `rhs`. `node` names an ASDL constructor or product and binds
  all its fields. `list` creates a sequence and `cons` prepends an element
  to a sequence. A terminal captured as `identifier` must be an ASCII
  identifier of at most 64 bytes, as `int`/`uint` a decimal in the respective
  64-bit range, or as `bool` `true`/`false`.
- `lowering` maps each ASDL constructor/product to an `op` and binds each
  of that operation's slots to an ASDL field. The recognized shapes are:

  | `op` | Required slots |
  | --- | --- |
  | `program` | `name`, `functions` |
  | `function` | `name`, `params`, `result`, `body` |
  | `parameter` | `name`, `type` |
  | `let` / `let_typed` | `name`, `value`; `let_typed` also has `type` |
  | `return`, `integer`, `bool`, `neg`, `not` | `value` |
  | `var` | `name` |
  | `true`, `false` | none |
  | `add`, `sub`, `mul`, `and`, `or`, `xor`, `eq`, `ne`, `lt` | `left`, `right` |

Functions have one straight-line block ending in a value return. Names
are immutable within a function. The supported value types are the integer
types above and `bool`. No implicit type conversions are inserted. Integer
arithmetic uses the selected fixed width, `lt` uses the operands' signedness,
and `eq`, `ne`, and `lt` produce `bool`. The mapping does not support calls,
branches, memory, arbitrary IR operations, or arbitrary C/X-macro templates.

## Grammar and schema subset

The embedded Aurocks code reads productions, validates symbols and
recursion, and compiles its own byte-oriented regex automata. The Python
runtime interprets that data in scannerless mode. It uses longest-prefix
terminals, first-successful alternatives ordered by `%dprec` then source
order, and Aurocks' seed/tail handling for direct left recursion. Thus
alternatives must be ordered deliberately: an enclosing production will not
retry a nonterminal's shorter alternative if an earlier choice matched
locally. Supported grammar controls are `%start`, optional `%skip`,
`%empty`, and alternative `%dprec`. C prologues, epilogues, and semantic
actions are rejected, as are other directives and unsupported recursion.
The translator never generates, compiles, or executes C from its inputs.

ASDL products and sums may have named fields of supported scalar or node
types, optionally qualified `*` or `+`. ASDL attributes, optional fields,
parameterized containers, embedded C, and generator directives are not
supported. The script checks the schema using canonical `asdl.pl --check
--schema -`; it does not compile ASDL-generated C.

Input sizes, regex expansion, parse/AST depth, and parser work are bounded.
If validation, parsing, or lowering fails, the tool leaves any existing
output untouched. Other generator representations still consume their
existing `Program.i` formats directly; their `Translate.py` does not
pretend to translate source to those formats.
