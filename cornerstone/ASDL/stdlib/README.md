# Extended ASDL — Lua standard library

**61 implemented modules** for the proposed libstone Extended ASDL, targeting Lua 5.4. The installation tree is `cornerstone/ASDL/stdlib`. All standard-library implementation code is Lua; the Python in `tests/` is only a portable test launcher. No external Lua packages are required.

This is a source distribution, not a patch to the existing C11-generating `asdl.pl`. The native reflection API, logistics dispatcher, import parser, and production library loader must be supplied by Extended ASDL. A standalone test host is included so the library can be exercised now. See `API.md` for precise data representations, preconditions, and every module's interface.

## Installation and imports

Copy or merge `cornerstone/ASDL/stdlib/std` into the corresponding directory in your libstone checkout. Configure the path (an ordered colon-separated list):

```sh
export LIBSTONE_ASDL_PATH="$PWD/userlib:$PWD/cornerstone/ASDL/stdlib"
```

```asdl
.import JSON, SExpr, Binary from std.serialize;
.import FaultPattern, Inform, Warn, Error from std.diagnostics;
.import ConstFold, Inline, Curry, foldable! from std.rewrite;
.import Fresh, Named, Arrow, Unify, Generalize from std.types.hm;
.import Env from std.semantics.environment;
.import CheckCoverage from std.semantics.patterns;
.import Scope from std.scope;
.import Control from std.control;
```

Lua code within logistics or libraries uses `asdl.lib.require("std.graph.scc").Tarjan`. Dotted imports resolve to `std/graph/scc.lua`; search explicit directory roots in order, followed by the bundled stdlib. Every library begins with `LIB = asdl.lib.new "..."` (or `asdl.lib.derive "..."`) and ends with `asdl.signlib(LIB)` / `return LIB`. Libraries can be extended without editing their source:

```lua
LIB = asdl.lib.derive "std/diagnostics.lua"

function LIB.UnknownName(node, name)
    LIB.Error(LIB.FaultPattern("E100", "Unknown name '%s'"), node, name)
end

asdl.signlib(LIB)
return LIB
```

A `.lua` file in a path root can also be loaded by its filename, and derive accepts explicit relative/absolute paths. The production loader should use canonical realpaths for caching and cycle detection, run each chunk in a distinct environment containing `asdl`, verify the returned signed `LIB`, and preserve inherited exports without mutating the parent. Signing is interface finalization, not cryptographic authentication or a sandbox. Libraries are trusted executable Lua. Export tables nested inside the library are intentionally mutable by Lua; treat them as read-only by convention.

## Contents

| Area | Modules | Implementations |
|---|---:|---|
| Core | 8 | Tree traversal, diagnostics, serialization, scope/capture, control, rewriting, expression IR, CFG |
| Collections | 7 | Vector, set, multimap, ordered map, worklist, bitset, priority queue |
| Graphs | 7 | Directed graphs, reachability, topological order, SCCs, dominators/frontiers, natural loops, Dijkstra |
| Types | 5 | Hindley–Milner, first-order unification, subtyping, constraints, lattices |
| Semantics | 7 | Bindings, environment, inheritance, pattern coverage, closures, effects, control flow |
| Analysis | 8 | Dataflow, liveness/interference, reaching definitions, def-use, escape, call graph, abstract interpretation, use counts |
| Transform | 8 | ANF, CPS, closure conversion, lambda lifting, SSA/phi lowering, constant folding, inlining, dead-code elimination |
| Match | 3 | Structural matching, first-order unification, pattern matrix and decision DAG |
| Format | 4 | Pretty printer, DOT, Mermaid, HTML |
| Testing | 4 | Assertions, fixtures, deterministic property tests/shrinking, snapshots |
| **Total** | **61** | |

`examples/` also contains the earlier MiniRuby schema and its two user libraries (not counted as standard-library modules).

## Running the tests

From the extracted archive root:

```sh
python3 tests/run.py
python3 tests/run_miniruby.py
```

The first command runs all modules against `tests/host.lua`, a small test adapter. It uses a system Lua 5.4 executable if available, otherwise a Lua 5.4 shared library via Python ctypes. The second runs the earlier MiniRuby example's 14 logistics using a separate mock host and the new stdlib; it uses the shared library. Tests cover loading/signing/derivation, collection and graph algorithms, HM polymorphism and occurs checking, recursive ADT coverage and guards, JSON and binary round trips, lexical captures, C3 inheritance, expression rewrite equivalence under an independent evaluator, SSA phi insertion/lowering, liveness, dataflow, and formatting/testing helpers. `TEST-RESULTS.txt` records the execution performed while packaging.

## Scope and design constraints

Compiler algorithms cannot infer the semantics of arbitrary user-defined ASDL constructors. This distribution implements them over explicit reusable contracts:

- Native AST reflection: tree traversal, generic matching/rewriting, serialization and diagnostics, plus adapters for Mini and MiniML's specific constructor shapes.
- `std.ir`: a pure, unary, call-by-value expression language with lexical scope, nonrecursive let, tuples, primitive operations and boolean branches. ANF, CPS, closure conversion, lifting and inlining operate on this representation.
- `std.cfg`: basic-block IR with explicit defs, uses and terminators. SSA conversion, phi lowering and graph/dataflow analyses operate on this representation.
- `std.match.decisiontree`: typed constructor/literal pattern matrix, open and finite domains, guards and ordered matching. MiniML supplies a concrete adapter.
- Nominal type environments, graph edges, effect summaries and call/escape targets are provided by the source-language logistics or lowering pass. Dynamic dispatch, effects, exception semantics and arbitrary AST inlining are not guessed by the library.

This is an implemented reference standard library for the proposed API, not proof of production integration or compiler correctness for every input. Recursive algorithms assume manageable acyclic trees/terms (graphs may contain cycles). The test loader uses lexical path normalization, not native realpath or a secure sandbox. Large untrusted inputs need separate resource controls. The `std.serialize.Binary` format is a versioned JSON transport, not existing Zephyr ASDL pickling. No repository files were modified.
