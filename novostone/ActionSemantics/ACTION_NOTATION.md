# Notation classification

This is an executable **dialect**, not full classical Action Notation compliance.
The reference is Peter D. Mosses, *Action Semantics* (1992), and Mosses/Watt's
Action Semantics framework; see the references linked in README.

| Facet | Implemented dialect facilities |
| --- | --- |
| Basic | complete, fail, give, check, deterministic sequencing |
| Functional | transient tuples, arithmetic and comparison |
| Declarative | bind, hence, furthermore, lexical closure capture |
| Imperative | cells, contents, integer store, tagged put |
| Procedural/reflective extensions | closure/abstraction, enact/apply, letrec, unfolding |
| LibStone control extensions | return/tailcall, tailunfold, callcc/resume, if/while, generators |
| Communicative / modern asynchronous | Not implemented |

`otherwise` is deterministic failure fallback, **not** nondeterministic choice.
`and then` is sequential, **not** parallel execution.
`abstraction` currently aliases lexical closure creation; it is not a distinct
uncaptured action value. `yield_value` is synchronous iterator suspension, not
scheduler yield. Call/cc is a LibStone one-shot control extension.

Grammar is authoritative in `ActionSemantics.g`; ASDL node shapes are in
`ActionSemantics.asdl`. See README for precedence and legacy constructs and
SEMANTICS for the extended operational rules. None of the proposed but
unimplemented task/module/object/effect keywords are accepted as fake primitives.
