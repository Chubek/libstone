# API and representation reference

All paths below are dotted names loaded with `asdl.lib.require`. A module returns a signed export mapping. Lua sequences are dense one-based arrays; sets are tables mapping elements to `true`. Unless otherwise stated, functions return a new value (graph/collection mutation methods are explicit exceptions). AST transformations return replacement roots; keep the returned root. For generic algorithms, callbacks must be pure or obey the documented contract. Names and variable IDs are strings unless stated otherwise.

## Required host interface

| Host primitive | Contract |
|---|---|
| `asdl.lib.new(name)` | Create an unsigned export mapping. |
| `asdl.lib.derive(path)` | Load a signed parent and copy its export mapping into an unsigned library. |
| `asdl.lib.require(name_or_path)` | Resolve and load a library; returns signed exports. |
| `asdl.signlib(lib)` | Validate and finalize exports. |
| `asdl.isnode(value)` | True only for actual live AST handles; accepts any Lua value. |
| `asdl.type(node)`, `asdl.tag(node)` | Declared type name and constructor name (nil for products). |
| `asdl.fields(node)` | Ordered descriptors `{name, cardinality, value}`; cardinalities `1`, `?`, `*`, `+`. |
| `asdl.destruct(node)` | Positional field values, preserving absent optional nils. |
| `asdl.children(node)` | Immediate AST children only, ordered by fields and sequence indices. |
| `asdl.make(type,tag,fields,origin?)` | Construct and validate a node, taking optional origin span; must not copy stale semantic annotations. |
| `asdl.map_children(node,fn)` | Transform immediate AST children, preserving their declared types and structural order; return original when unchanged. |
| `asdl.clone(node)` | Deep structural clone. |
| `asdl.annotate(node,key,value)` | Write session-local annotations; `annotation`, `annotations` are corresponding getters. `annotations` optional for serialization. |
| `asdl.location(node)` | Source span or nil. |
| `asdl.diagnostic.emit(record)` | Emit `{severity,code,message,location}` with optional string `name` and `group`. |
| `asdl.diagnostic.count(severity)` | Count diagnostics in the current session. |
| `asdl.logistics(node,...)` | Call the node type's registered hook; no implicit traversal. Used by examples. |
| `asdl.option(name)` | Optional host settings. Used by examples. |

The test host exposes `lib.searchpath`, `lib.resolve`, `lib.loaded`, `lib.exports`, `lib.describe`, `lib.issigned`, and testing-specific `define`, `hook`. It is not a production implementation of the proposed native `asdl` library. Metadata beyond the required API (schema reflection, native arena management, library version/dependency declarations) belongs in that future host, not in this Lua stdlib.

## Core

### `std.diagnostics`
`FaultPattern(code,format,metadata?)` creates a template. Its optional metadata
table accepts `name` (a stable message identifier) and `group` (a label shared by
related messages). Both are nonempty, case-sensitive strings, matched exactly
within the current session. They are independent of the display `code` and
are preserved on emitted records. `Error`, `Warn`, `Inform` and
`Fatal(pattern_or_string,node?,...)` emit diagnostics; `Fatal` then throws.
`Checkpoint()` records the error count; `FailedSince(mark)` detects errors
since that checkpoint. These compose with derived libraries.

`Disable(selector?)` and `Enable(selector?)` accept a severity (`"error"`,
`"warning"`, or `"info"`), `{group = "..."}`, or `{name = "..."}`. A name
setting overrides a group setting, which overrides the severity setting.
Omitting the selector sets all severities and clears all group and name
overrides. `IsEnabled(severity, pattern?)` checks the effective setting for
a pattern; omitting the pattern checks only the severity.

```lua
local D = asdl.lib.require("std.diagnostics")
local unused = D.FaultPattern("W01", "Unused binding %s",
    {name = "bindings.unused", group = "bindings"})
D.Disable({group = "bindings"})
D.Enable({name = "bindings.unused"}) -- keep just this message enabled in the group
D.Warn(unused, nil, "x")
```

All messages start enabled in each session. Suppressed messages return nil
without formatting or adding a host diagnostic record. Suppressed errors still
count toward `Checkpoint()` and `FailedSince()`, and `Fatal` still throws.
These controls only affect `std.diagnostics` and its inherited exports, not
direct calls to `asdl.diagnostic.emit`.

### `std.tree`
`Tree.walk(root,enter?,leave?)` walks in schema order; enter returning false prunes the subtree and skips leave. `Tree.collect(root,predicate,descend?)` finds matching nodes; `Tree.any(root,predicate)` short-circuits; `Tree.fold(root,initial,combine)` folds postorder. Input must be an acyclic AST. Shared subtrees are visited once per path.

### `std.scope`
`Scope.new(parent,kind,owner,ctx)` creates a lexical scope. Shared `ctx.next_binding` supplies IDs. `declare(scope,name,kind)` returns a binding or nil and the duplicate; `lookup` does not record captures; `resolve` records captures on intervening scopes; `predeclare` reuses visible binding or declares a local; `finish(scope)` writes sorted `locals` and `captures` annotations on owner. Language policy decides which scopes have lexical parents and when to predeclare. Captured mutable cells are allocated by downstream lowering, not by this analysis.

### `std.control`
`Control.enter(parent,kind,owner,ctx)` assigns a control ID; `Control.target(frame,operation,policy?)` returns the nearest allowed frame, respecting barriers. Default policy is MiniRuby-compatible (Return/Break/Next/Retry/Yield/Super). Custom policy is `{targets={Operation={kind=true}}, barrier=function(frame,operation)...end}`; `barrier` returns true to stop lookup after testing current frame. Control IDs are lexical, not activation IDs.

### `std.serialize`
`JSON(value,options?)`, `DecodeJSON(string,options?)`, `FromJSON(string,options?)`, `SExpr(value)`, `Binary(value,options?)`, `FromBinary(bytes,options?)`. JSON node objects have reserved `$type`, optional `$tag`, and schema fields; sequence fields are arrays, absent optionals null. `options.annotations=true` includes/restores `$annotations` when host supports it. Decoding validates JSON syntax, UTF-8, surrogate pairs, duplicate keys and depth (default 256). `Null` is a sentinel; `Array(xs?)` disambiguates empty arrays from objects. Generic JSON numbers must be finite; floats are formatted to round-trip Lua binary64, including negative zero. `FromJSON` reconstructs AST objects through `asdl.make`, omits null node fields, rejects null elements in AST sequence fields, and preserves null and array markers in ordinary JSON values. Binary is `ASDL\x01` + big-endian unsigned 64-bit length + UTF-8 JSON. SExpr is a human-readable encoding, not a Scheme reader or Zephyr pickle. Metadata is optional; source locations are not serialized automatically. Cyclic values are rejected. Raw binary ASDL primitives need a language adapter to encode as UTF-8 or another representation.

### `std.rewrite`
`Rewrite.bottom_up(root,rule,source_map?)`, `top_down(root,rule,source_map?)`, `pipeline(root,...)`, `rule(type,tag,fn)`, `fixed_point(root,pass,equal,limit?)`. A rule returns nil (unchanged) or same-type node. The optional map records transformations of handles, including nodes rebuilt by `asdl.map_children`. With provenance enabled, a new replacement must not be reused for multiple inputs. Top-down rules must not introduce infinitely expanding subtrees; fixed point limits passes.

`Curry(root)` handles MiniML's LambdaMany and ApplicationMany. `ConstFold(root,adapter?)` handles the Mini literal operator AST conservatively; default does not fold division/modulo where source semantics were unspecified. For IR, delegates to `std.transform.constfold` (adapter argument becomes primitive map). `LIB['foldable!'](node)` requests folding. `Inline(root,target,adapter?)` supports a safe Mini nullary, no-locals, single-literal-return function or user-provided AST adapter; the IR overload beta-inlines direct lambda applications. `BetaReduce`, `EtaReduce`, `DeadEliminate` are IR operations. Generic AST optimizers must receive language adapters; MiniRuby Send is not treated as primitive arithmetic.

### `std.ir`
Constructors `Const(value)`, `Var(name)`, `Lambda(param,body)`, `App(fn,arg)`, `Let(name,value,body)`, `If(cond,yes,no)`, `Prim(op,args)`, `Tuple(args)`. Each is a plain table with corresponding lowercase `tag` and named fields. Values are numbers, booleans or strings; tuples and closures are produced by evaluation, not mutable constant tables. Evaluation is lexical, left-to-right call by value, let is nonrecursive, if requires a boolean. Functions are unary and pure; primitive operations may fail and must not be evaluated early unless safe. `Map(e,fn)` maps immediate expression children; `Free(e)` and `Names(e)` give sets; `Freshener(e,prefix?,extra?)` produces unique strings; `Substitute(e,name,value)` is capture-avoiding; `Alpha(e)` makes binders globally unique; `Atomic(e)` recognizes constants, variables, lambdas. The IR is finite/acyclic. It is not a replacement for a source language's AST.

### `std.cfg`
`New(entry,blocks,order)` validates and returns a CFG; `Validate(cfg)` checks basic structure and branch targets; `Graph(cfg)` constructs the directed graph; `Copy(x)` copies acyclic plain tables; `Instructions(block)` returns instructions followed by terminator.

```lua
local cfg = {
  entry = 'entry', order = {'entry', 'exit'},
  blocks = {
    entry = {
      instrs = {{op='const', defs={'x'}, uses={}, value=42}},
      term = {op='jump', defs={}, uses={}, targets={'exit'}}
    },
    exit = {instrs={}, term={op='return', defs={}, uses={'x'}, targets={}}}
  }
}
```

Definitions and uses are complete lists of variable IDs, not inferred from opcodes; branch destinations occur only in `term.targets`. Model parameters as entry definitions. Instructions are plain data and must not hide variable operands elsewhere. CFG analyses treat all syntactic edges as possible. IDs should be strings or positive integers. An SSA CFG has `ssa=true`, `block.phis={{var,dest,args={ [pred]=version }}}` and `undef={ original=version }` for undefined incoming values.

### `std.passes`

`New(specs)` validates a list of named artifact-producing passes. A spec has `name`, optional `requires` list, `run(input,dependencies,context)`, and optional `cache`, `key(input,context)`, `effects`. `Plan(manager,targets?)` returns a deterministic dependency-first order, including transitive dependencies; omitted targets mean all passes in declaration order where dependencies permit. Missing names and cycles are errors. Passes run serially; dependencies are the only scheduling constraints.

`Run(manager,input,{targets?,revision?,context?})` returns a name-to-result map and `{order,ran,hits,passes={ [name]={cached=bool} }}`. Result/dependency maps omit nil values, but nil results are memoizable. The dependency map contains only direct dependencies, and inputs and cached artifacts must not be mutated. `Clear(manager)` invalidates all cached entries. Caches hold the latest entry per pass; a newer entry produced by any prerequisite invalidates dependents, including when values compare equal. Failed runs do not commit staged cache changes; side effects are not rolled back.

By default a pass is not cached. `cache=true` requires explicit effects with no writes or unknown effects. A cached pass uses its `key` callback or the run's `revision`, compared by Lua raw equality. The caller must change the key whenever any relevant input, context, or externally read resource changes; for incrementality across a change, use a per-pass key over the relevant immutable input. Effects use `{reads=set,writes=set,unknown=bool}`; omitted effects are unknown. `Conflict(a,b)` reports write/read and write/write conflicts, treating unknown as conflicting with nonempty summaries. It does not create dependency edges or schedule parallel execution.

```lua
local P = asdl.lib.require("std.passes")
local SSA = asdl.lib.require("std.transform.ssa")
local L = asdl.lib.require("std.analysis.liveness")
local manager = P.New {
    {name="ssa", cache=true, effects={}, run=SSA.ToSSA},
    {name="liveness", requires={"ssa"}, cache=true, effects={},
     run=function(_, deps)
         local live_in, live_out = L.Liveness(deps.ssa)
         return {input=live_in, output=live_out}
     end},
}
local results, stats = P.Run(manager, cfg, {revision=1})
```

### `std.sourcemap`

`New()`, `Record(map,generated,input)`, `Origin(map,generated)` (direct origin or nil), `Trace(map,generated)` (oldest origin and path, including `generated`), `Compose(previous,next_map)` (new combined map). Provenance is keyed by Lua identity for handles or by caller-chosen stable IDs. Each output has at most one direct origin; self-maps are ignored and conflicting origins rejected. Trace and composition detect cycles. Maps retain their keys and values, so keep them only as long as their provenance is needed. This tracks provenance, not line/column translations; use `asdl.location(origin)` for a native node.

## Collections

| Module | Export and methods |
|---|---|
| `std.collections.vector` | `Vector`: new, push, pop, insert, remove, map, filter, fold, slice, concat, index |
| `std.collections.set` | `Set`: new, copy, add, remove, has, union, intersection, difference, subset, equal, size, values(optional comparator) |
| `std.collections.multimap` | `MultiMap`: new, add, get(copy), remove(first match), clear(key) |
| `std.collections.orderedmap` | `OrderedMap`: new, set, get, remove, items(iterator in insertion order) |
| `std.collections.worklist` | `Worklist`: new, push(deduplicate pending), pop(FIFO), empty |
| `std.collections.bitset` | `BitSet`: new, add, remove, has, union, intersection, difference, values(sorted); sparse 32-bit words, nonnegative integer indices |
| `std.collections.priorityqueue` | `PriorityQueue`: new(less?), push, pop, peek; binary min-heap |

Mutation methods operate in place. Keys/items must be non-nil; sequences may contain false. `Set.values` iteration order is unspecified without comparator. Iterators should not be used concurrently with map mutation.

## Graphs

`std.graph.directed.Graph.new(nodes?,edges?)` gives `{nodes,out,incoming}` with stable insertion order, where each edge is `{from,to}`. `add(g,v)`, `edge(g,a,b)` mutate, deduplicating edges; `successors`, `predecessors` expose read-only-by-convention adjacency arrays; `reverse(g)` constructs a new graph. Use non-nil, truthy hashable IDs; do not mutate adjacency arrays directly.

| Module | Exports |
|---|---|
| `std.graph.reachability` | `Reachable(g,starts)` → set, BFS order; `Reachability(g)` all pairs; `Path(g,a,b)` shortest unweighted path or nil |
| `std.graph.topology` | `Topological(g)`/`Topology` → order or nil, blocked vertices if cycle |
| `std.graph.scc` | `Tarjan(g)`, `Kosaraju(g)` → arrays of SCC arrays; `Condensation(g,groups?)` → DAG, vertex→component map, groups |
| `std.graph.dominators` | `Dominators(g,entry)` → vertex→set, reachable only; `ImmediateDominators(g,entry,d?)`; `Frontier(g,entry,d?,idom?)` |
| `std.graph.loops` | `NaturalLoops(g,entry)` → `{header,latch,body=set}` for each dominance back edge (irreducible regions are not natural loops) |
| `std.graph.shortestpath` | `Dijkstra(g,start,weight(from,to))` → distance and predecessor maps, finite nonnegative weights only |

## Types

`std.types.hm`: `Fresh`, `Named(name,args?)`, `Arrow`, `Tuple`, `Prune`, `Unify(a,b,node?)`, `Free(t,out?)`, `Mono`, `Generalize(env,t)`, `Instantiate(scheme)`, `CtorScheme(quantified,argtypes,result)`, `InstantiateCtor`, `Pretty`. Type variables are identity-bearing tables with mutable union links. Occurs checking rejects infinite types. Unification mutates on success and may leave partial substitutions on failure: abort that inference session, or use a separate solver state for speculation. Quantified scheme vars must not be directly unified; instantiate first. Generalization implements pure HM let-polymorphism, not a value restriction, higher-rank types or polymorphic recursion.

`std.types.unification`: `Var(name)`, `Term(symbol,args?)`, `Apply(term,substitution)`, `Unify(a,b,initial?)` → substitution or nil,error. Initial substitution must be acyclic. `std.match.unification` reexports the same implementation.

`std.types.constraints`: `New()`, `Equal(constraints,a,b,node?)`, `Solve(constraints)` using HM unification.

`std.types.subtyping.IsSubtype(a,b,parents)` checks nominal string types, Top/Bottom, contravariant arrow domains and covariant codomains/tuples. `parents` maps names to arrays of immediate supertype names. Not a complete structural subtyping calculus.

`std.types.lattice`: `Powerset(universe)`, `Finite(elements,leq)` → lattice objects `Top()`, `Bottom()`, `Join(a,b)`, `Meet(a,b)`, `Leq(a,b)`, `Equal(a,b)`. `Join(l,a,b)`, `Meet(l,a,b)`, `Top(l)`, `Bottom(l)` convenience exports. Finite input must be a partially ordered lattice; incompatible bounds raise an error.

## Semantics

- `std.semantics.environment.Env`: `new(parent?)`, `bind_value(env,name,scheme)`, `lookup_value(env,name,node?)`, `declare_type(env,name,arity,node?)`, `type_arity`, `declare_ctor(env,name,scheme,node?)`, `lookup_ctor`, `constructors(env,type_name)`, `free(env)`, `prelude(language?)`. Environment extensions are persistent; values can shadow, nominal type/constructor declarations reject duplicates. MiniML prelude has int/real/bool/string/unit and monomorphic integer operators.
- `std.semantics.bindings`: `Scope`, `Resolve(scope,name,node?)`, `MarkRead(binding)`, `MarkWrite(binding)`, `Unused(scope)`; reads/writes must be marked by caller.
- `std.semantics.inheritance`: `Linearize(classes,name)` C3, `Lookup(classes,methods,name,method)` → method, defining class. `classes[name]` = ordered parent names; `methods[name][method]` = entry. Rejects cycles and inconsistent parent orders.
- `std.semantics.closures`: `FreeVariables(ir)`, `Captures(lambda)` → sorted names.
- `std.semantics.effects`: `Effects(root,local_effects,children?)` unions set summaries. `Pure(effects)`, `Allowed(effects,allowed)`. Unknown local effect defaults to `unknown`. Default children use native AST reflection; callback must exclude latent bodies if modeling immediate effects.
- `std.semantics.patterns`: `CheckCoverage(env,type,arms)` adapts MiniML's pattern and arm constructors to the matrix solver, returns `{exhaustive,witness,redundant}` (redundant entries are arm nodes). Constructor parameters follow declared type-parameter order. Witnesses are symbolic patterns, not necessarily concrete values; guarded arms conservatively may fail. Open literal domains are incomplete unless covered by wildcards. `Coverage` is the generic matrix solver.
- `std.semantics.controlflow`: `Control`, `Reachable(cfg)`, `Unreachable(cfg)`, `Dominators(cfg)`, `Loops(cfg)`, `PostDominators(cfg)` relative to synthetic exit (non-exiting regions omitted; not a termination proof).

## Matching

`std.match.structural`: `Any()`, `Capture(name,pattern?)`, `Node(type,tag,fields?)`, `Match(pattern,value)` → capture table or nil, `Equal(a,b)`, `Replace(root,pattern,build(captures,node))`. A repeated capture must match structurally; absent nil captures do not have a key in returned table. Plain table patterns match exact keys; AST node patterns can constrain a subset of named fields. Ignore annotations in structural equality. Input trees/tables must be acyclic.

`std.match.decisiontree`: pattern IR `{tag='wild'}`, `{tag='lit',value=...}`, `{tag='ctor',name=...,args={...}}`, `{tag='or',lhs=...,rhs=...}`. Domains are arrays of `{pattern=constructor_or_literal_head, domains=child_domains}`; `{}` means open/infinite; a function returning a domain enables recursive ADTs. Input patterns must be well-typed and correct arity. `Useful(matrix,query,domains)` returns symbolic uncovered witness vector or nil; `Coverage(arms,domain)` returns report with redundant indices. Arms are `{pattern,guarded=bool}` for coverage. `Build(arms)` creates an ordered decision DAG; `Run(tree,subject,view)` evaluates it; `Compile(arms,view)` returns callable matcher and DAG. Runtime arms use optional `guard(subject)` and `action(subject)`; view returns constructor name and dense argument array or nil for non-constructors. View must be pure. Runtime returns matching arm index and action result(s) or nil. Binding extraction is performed by caller/action (coverage deliberately erases bindings).

## Analyses

| Module | API and results |
|---|---|
| `std.analysis.dataflow` | `Forward(g,spec)`, `Backward(g,spec)` → in,out maps. Spec: bottom(), join(a,b), equal(a,b), transfer(vertex,value), optional boundary map, edge(from,to,value), limit. `statistics=true` adds a third `{steps,changes}` return value. Must be monotone on a converging domain, values treated immutably. `FixedPoint(initial,step,equal,limit?)`. |
| `std.analysis.liveness` | `Liveness(cfg)` → live-in/out sets; `LiveIn`, `LiveOut`, `Interference` (conservative symmetric graph). SSA phi uses are edge-specific. |
| `std.analysis.reachingdefs` | `ReachingDefinitions(cfg)` → in,out,definition records, per-instruction IDs. Ordinary or phi-lowered CFG only. |
| `std.analysis.defuse` | `DefUse(cfg)` → `{uses,definitions,by_definition}`; uses carry reaching def IDs. Ordinary CFG only. |
| `std.analysis.escape` | `EscapeAnalysis(graph,roots)` → reachable set and order; `Escapes(graph,roots,v)`. Caller models containment, aliases and escape roots. |
| `std.analysis.callgraph` | `CallGraph(functions,calls)`; each function has name, callback returns conservative possible callee names. Unknown external targets can be vertices. |
| `std.analysis.abstractinterp` | `Interpret(g,lattice,transfer,boundary?,limit?)` → in/out; `ConstantLattice()` flat constant domain with Top/Bottom/Constant/Join/Meet/Equal. |
| `std.analysis.usecounts` | `UseCounts(cfg)` counts instruction, terminator and phi operands. |

## Transformations

Except SSA, these use `std.ir`. They return fresh expression structures as needed. No mutation of arbitrary native ASTs, effect inference, or concrete object model is assumed.

- `std.transform.anf.ANormalize(expr)` alpha-renames and introduces lets for non-atomic operands, preserving left-to-right order and conditional laziness.
- `std.transform.cps.CPSConvert(expr)` converts to `lambda k. ...`; transformed functions take ordinary arg then continuation. Invoke result with a final continuation. A CPS caller must adapt external functions to the calling convention.
- `std.transform.closureconv.ClosureConvert(expr)` returns `{body,functions}`. A lifted function is `{name,param,captures,body}`. `closure` nodes contain code name and capture expressions (captured values available by name inside code); `invoke` calls closure with argument. Pure immutable bindings only. For mutable bindings, a language adapter must first box them.
- `std.transform.lambdalift.LambdaLift(expr)` returns same program representation, adding `formals` (captures then parameter) to each lifted function and replacing direct known closure calls with `lifted_call {code,captures,arg}`. Escaping function values retain closure records. This is lifting for nonrecursive unary IR, not an arbitrary mutually recursive functional language.
- `std.transform.constfold.ConstFold(expr,primitives?)` evaluates constant primitive applications safely under caller-provided pure semantics; default `Primitives()` includes arithmetic, comparisons, not. Defaults follow Lua number/arithmetic semantics (division/mod by zero explicitly fail). Keeps operations that throw or cannot be folded. Boolean constant branches are simplified.
- `std.transform.inline.Inline(expr,limit?)`/`BetaReduce(expr)` use capture-free let-based inlining of direct lambda applications (argument evaluated once). `EtaReduce` only reduces a statically known lambda function value where binder is not free.
- `std.transform.deadcode.DeadEliminate(expr)` eliminates unused constant/lambda bindings and constant boolean branches, not possibly failing primitive operations or calls.
- `std.transform.ssa.InsertPhi(cfg)` uses iterated dominance frontiers. `ToSSA(cfg)` inserts phis and renames by dominator-tree traversal, explicitly recording undefined incoming versions. Input CFG must have predecessor-free entry, all blocks reachable, no existing phis. No liveness-pruning of phis. `ValidateSSA(cfg)` checks unique definitions, complete phi operands and dominance, including predecessor-edge phi uses; returns true or raises an error. `FromSSA(cfg)` splits incoming edges and uses temporary copies to implement simultaneous phi assignments; leaves `undef` map for backend handling. Does not coalesce copies or allocate registers.

## Formatting/testing

- `std.format.pretty`: `Text(s)`, `Line()`, `Concat(...)`, `Nest(n,doc)`, `Group(doc)`, `Join(sep,docs)`, `Render(doc,width?)`. Group fits by byte length; line is space in flattened group and newline otherwise. Text may not contain newline.
- `std.format.dot`: `DependencyGraph(graph,label?)`, `ASTGraph(root)`, `ControlGraph(cfg)` → DOT text.
- `std.format.mermaid.DependencyGraph(graph,label?)` → Mermaid source; no renderer required.
- `std.format.html`: `Escape`, `Pre`, `AST(root)` (escaped S-expression in pre element).
- `std.testing.assertions`: `Equal(a,b,msg?)`, `Structural(a,b)`, `Throws(fn,fragment?)`.
- `std.testing.fixtures`: `Node(type,tag,fields?)`, `Run(cases)` with `{name,run}` records → counts and failure records.
- `std.testing.property`: `RNG(seed)` returns deterministic inclusive integer generator; `Generate(gen,seed,size?)`, `Integer(lo,hi)`, `Array(gen)`, `Shrink(value,shrink,fails,limit?)`, `ForAll(gen,property,opts?)` returns true or false and counterexample report. Predicate must return truthy on success. Options: runs, seed, size, shrink. Shrinker returns array of candidates; greedily accepts failing reductions within budget.
- `std.testing.snapshot.Snapshot(key,value,read,write?,update?)` compares values using Lua equality (use serialized strings for trees), optionally updates via callback; no implicit filesystem writes.
