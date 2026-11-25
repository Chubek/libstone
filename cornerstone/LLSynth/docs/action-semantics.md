# ActionSemantics rule facets

Use `'semantics actionsemantics(...) -> Type:` in place of the ordinary
`'semantics:` facet to write a rule's interpretation in
[NovoStone ActionSemantics](../../../novostone/ActionSemantics/README.md):

```text
.token Number: Int = /-?[0-9]+/
.rule Start
'syntax: value:Number EOF
'semantics actionsemantics(value: Int) -> Int:
  (allocate cell then
    (store value in given(1) and then give contents(given(1)) + 1))
.end
```

The body is a single NovoStone action, not an LLSynth statement block or a
complete `.acsem` file. Omit the equation's terminating semicolon. The action
can begin on the header line or continue onto following lines. As with other
facets, it ends at the next facet, at `'end`, or at the end of the rule.
Rules using this syntax can coexist with ordinary `'semantics:` rules, but
cannot also have an ordinary `'semantics:` facet of their own. The rule's
`'syntax`, `'lower`, `'analyze`, and `'emit` facets are unchanged.

The signature is the boundary between the two languages:

| LLSynth type | Input capture passed to the action | Value returned on completion |
| --- | --- | --- |
| `Int` | A signed 32-bit integer (checked when the rule runs) | One integer, widened to LLSynth `Int` |
| `String` | An ASCII identifier of 1–64 bytes (`[A-Za-z_][A-Za-z_0-9]*`) | One identifier token, returned as a string |
| `Void` | Not an input type | No transients, returned as LLSynth's empty value |

List only the captures the action needs; use `()` if there are none. Each
listed name must be a capture in every syntax alternative. Write `value`,
not `$value`, in the action. Captures can come from terminals or from
already interpreted child rules. Grouped, optional and repeated captures
cannot cross the boundary. LLSynth checks known capture types during
synthesis; dynamically typed values and result arity/tag are checked at
execution.

Each invocation uses a fresh NovoStone machine with its own bindings and
store. It cannot call LLSynth functions, mutate LLSynth state or access
LLSynth syntax nodes. Thus NovoStone `evaluate(field)` does not invoke
child rules; their result values are what the facet sees. Data declarations,
rewrite-bank declarations and other `.acsem` top-level forms are not
allowed in the inline action. Failure or escape that is not handled by the
action, an invalid boundary value, and resource exhaustion surface as
`ACSEM` runtime diagnostics at the corresponding LLSynth rule.

## Build and run

`acsem` is only needed by LLSynth at synthesis time, including `--check`.
Either put the NovoStone executable on `PATH` or pass `--acsem PATH`.
The generated interpreter uses the real generated NovoStone machine
without having to launch `acsem` or load a `.acsem` file at runtime.

From the repository root:

```sh
cmake -S novostone -B build-novostone
cmake --build build-novostone --target acsem

cornerstone/LLSynth/scripts/llsynth.pl --mode interpret \
  --acsem build-novostone/ActionSemantics/acsem -o actions.py \
  cornerstone/LLSynth/examples/typed/action-semantics.llsynth
printf 'double 21\n' | python3 actions.py --interpret
# 42
```

Python output includes the machine in the generated tool or package.
For C99 and C11, compile the generated `*.actionsemantics.c` companion
alongside the other outputs, even when using `--split none`. When targeting
C++17 or C++20, compile the companion as C and link the object with the
generated C++ code. For example, if the generated main file is `actions.c`:

```sh
cc -std=c99 actions.c actions.actionsemantics.c -lm -o actions
```

NovoStone's synthesis and runtime limits apply; in particular, at most 16
captures per rule and 256 delegated rules per specification are supported
by this bridge. NovoStone chooses the dialect from the actions in the
specification as a whole.
