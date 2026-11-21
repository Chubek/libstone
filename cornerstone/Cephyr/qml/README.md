# Cephyr QML

The Cephyr `QML.llsynth` grammar serves two tools:

- **Native rules** are directive-only fragments. `NativeCanonicalize.qpass`
  supplies builtin identities for pure integer SSA operations, and
  `NativeStrengthReduce.qpass` reduces wrapping multiplication by two to
  addition. An integer pattern leaf can match a literal discovered through a
  `const` definition; a metavariable binds the operand itself. Native legality
  and phase execution are checked by the host, not by the syntax compiler.
- **Expression programs** end in `run TREE;`. The older expression-only
  `.qpass` files listed in `bundle.py` are not native rules. Bundle them with
  `python3 cornerstone/Cephyr/qml/bundle.py -o demo.qml` and run
  `python3 qml_frontend.py demo.qml`. `runtime.py` is the independent
  expression reference.

## Native source and compilation

```qml
# Rule and pass names are identifiers.
rewrite canonicalize add_zero : add(?x, 0) => ?x;
reject canonicalize forbid_sub : sub(_, _);
pass canonicalize;
```

`TREE` is a constructor such as `add(?x, 0)` or `empty()`, a metavariable
`?name`, a signed decimal integer, a bare symbol name, `true`, `false`, or `_`
(wildcard). Identifiers match `[A-Za-z_][A-Za-z0-9_]*`; keywords are reserved.
Literals must fit signed 64-bit integers. Whitespace and `#` line comments
are ignored. A rewrite replacement may use only metavariables bound by its
pattern and may not contain wildcards. The wrapper rejects `run` before
evaluating any expression. It reads regular-file fragments independently and in
argument order, with aggregate limits of 64 KiB, 128 directives, and 4096 tree
terms. Identifier length, tree depth (including the root), and constructor
arity are limited to 64, 32, and 32 respectively.

Native MachineIR rules must be flat and operate on `i64`: `copy`, `add`, `sub`,
`mul`, `and`, `or`, `xor`, `shl`, `lshr`, `ashr`, `eq`, `slt`, and `ult`.
Operands in a match can be metavariables, integers, or wildcards. A repeated
metavariable requires the same SSA operand. A replacement is a bound
metavariable, an integer result, or one supported operation over bound
operands and integers already matched by the pattern. Calls, memory
operations, trapping operations, and control flow are not rewrite sites.

From the repository root:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T python3 \
  -o qml_frontend.py cornerstone/Cephyr/QML.llsynth
python3 cornerstone/Cephyr/qml/compile_rules.py \
  --frontend qml_frontend.py --output builtin.qmb \
  cornerstone/Cephyr/qml/NativeCanonicalize.qpass \
  cornerstone/Cephyr/qml/NativeStrengthReduce.qpass
```

The wrapper uses the generated lexer, LALR parser and semantic actions; it
does not use the independent reference parser. It emits ASCII with LF line
endings, starting with `CEPHYR-QML-1`. Each following line is a directive,
in source order: `R PHASE NAME TREE TREE`, `X PHASE NAME TREE`, or `P PHASE`.
Trees are written as space-separated prefix tokens:

| Source term | Compiled tokens |
| --- | --- |
| `NAME(children...)` | `n NAME ARITY CHILDREN...` |
| `?NAME` | `m NAME` |
| integer | `i DECIMAL` (canonical signed decimal) |
| symbol | `s NAME` |
| `false`, `true` | `b 0`, `b 1` |
| `_` | `w` |
