#!/usr/bin/env python3
"""Generate the 40-chapter CCINT manual and MANIFEST.yaml. Standard library only."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
# title, capability, implementation status, header(s), specific explanation, technique, pitfall, exercise
TOPICS = [
('Design Goals','architecture','implemented','include/Weighstone/CCINT.hpp','CCINT is a compiler-centered collection of numerical abstractions. It separates mathematical integers, fixed-width integers, abstract values and statements about values, because an optimizer must not silently substitute one semantics for another.','Design typed entry points and require every transformation to state its semantic domain. Prefer conservative unknowns to invented facts.','A numerical fact detached from its IR assumptions may cease to be true after a control-flow change.','Trace an SSA constant through the component boundary and record the preconditions used.'),
('C++20 Setup','integration','implemented','CMakeLists.txt','The installed artifacts are headers: no Weighstone object library needs linking. Boost.Multiprecision supplies the variable-size integer backend, so consumers must have Boost headers available.','Compile a sample with -std=c++20 and -Iinclude -I. Add an INTERFACE CMake target to propagate requirements.','The current CMake install interface is a starting convention, not a full exported install/package implementation.','Compile examples/basic.cpp outside the CMake project using explicit include flags.'),
('Mathematical vs Machine Integers','semantics','implemented','include/Weighstone/CCINT/BitVector.hpp','An unbounded integer is a point in Z; a bitvector is a residue in Z/(2^w). Reassociation and cancellation laws may agree, but overflow flags, division and comparison can differ.','Preserve width and interpretation in every IR adapter, and make checked or wrapping behavior explicit at the call site.','Poison, traps, saturating arithmetic and undef are target-IR concepts not encoded by plain mathematical expressions.','Compare x+1 at the maximum unsigned value for 8 and 17 bit widths.'),
('Big Integers','arithmetic','implemented','include/Weighstone/CCINT/BigInt.hpp','BigInt aliases boost::multiprecision::cpp_int. No machine-sized bound is imposed by CCINT, so coefficients and loop counts can exceed 64 bits.','Use exact values in proofs and cast to a host integer only after proving representability.','Large shifts and deliberately huge integers can exhaust resources; input validation is the compiler integrator’s responsibility.','Calculate 2^257-1, multiply it by a large prime and recover a factor.'),
('Division and Remainders','arithmetic','implemented','include/Weighstone/CCINT/BigInt.hpp','The host C++ division operator truncates toward zero. CCINT additionally supplies floor_div, ceil_div and euclidean_mod, because affine index reasoning often requires floors and nonnegative residues.','Define the quotient convention before algebraic simplification. For positive m, 0 <= euclidean_mod(a,m) < m.','Negative divisors are accepted by floor_div and ceil_div, but euclidean_mod requires a positive modulus.','Tabulate quotient and remainder for +/-7 divided by +/-3 and verify identities.'),
('GCD and LCM','number theory','implemented','include/Weighstone/CCINT/BigInt.hpp','The Euclidean algorithm finds the nonnegative greatest common divisor. The lcm helper returns zero if either input is zero; useful for stride normalization and alignment compatibility.','Reduce fractions, eliminate common factors in constraints, and normalize modular periods.','Treat gcd(0,0)=0; attempting to divide a constraint by that value is invalid.','Build a stride checker that proves every address is aligned to gcd(base, step).'),
('Extended Euclid','number theory','implemented','include/Weighstone/CCINT/NumberTheory.hpp','extended_gcd returns g,x,y satisfying a*x+b*y=g, with g>=0. This witnesses solvability of elementary integer linear equations.','Use Bézout coefficients as witnesses in divisibility tests, not as machine integers.','The coefficients are not unique; never use them as canonical identifiers without normalization.','Verify the Bézout identity on mixed-sign inputs including zero.'),
('Modular Inverses','number theory','implemented','include/Weighstone/CCINT/NumberTheory.hpp','mod_inverse(a,m) finds x with ax congruent to 1 modulo m, provided gcd(a,m)=1. It returns optional to represent the absence of an inverse.','Use the inverse to solve affine modular equations after dividing away a common gcd.','A modular inverse does not exist for every pair; code must handle the empty optional.','Solve 7*x=3 mod 26 and show that 6*x=1 mod 10 has no solution.'),
('Chinese Remainder Theorem','number theory','implemented','include/Weighstone/CCINT/NumberTheory.hpp','chinese_remainder combines two residues even when moduli are non-coprime, provided they agree modulo the GCD. The answer is canonical modulo the LCM.','Use the result to combine alignment requirements from nested loops and address calculations.','An incompatible pair is unsatisfiable, not an unknown solution; propagate this as a contradiction.','Combine x=2 mod 3 and x=3 mod 5, then construct an incompatible pair.'),
('Exact Rationals','arithmetic','implemented','include/Weighstone/CCINT/Rational.hpp','Rational stores an integer numerator and a positive denominator, reduced by gcd. Its operations and ordering never round to floating point.','Use it for exact symbolic coefficients and intermediate matrix computations.','Division by a zero-valued rational throws; the caller must establish a nonzero divisor.','Compute 1/3+1/6 and verify comparison against 1/2.'),
('BitVector Representation','bitvectors','implemented','include/Weighstone/CCINT/BitVector.hpp','BitVector carries an explicit bit width and an unsigned canonical residue. signed_value reinterprets the same bits using two’s complement. Supported widths currently range from 1 to 1048576.','Store widths as part of values and reject operations between mismatched widths.','This implementation is correct-oriented rather than constant-time; do not use it for cryptographic secrets.','Display the signed and unsigned interpretations of 0x80 at width eight.'),
('Checked Arithmetic','bitvectors','implemented','include/Weighstone/CCINT/Checked.hpp','checked_add, checked_sub and checked_mul compute a wrapping result alongside separate signed and unsigned overflow flags. That information can discharge no-overflow preconditions.','Track which overflow predicate a transformation requires: unsigned and signed flags are not interchangeable.','Checking addition of a negative signed operand may differ dramatically from unsigned overflow.','Enumerate every pair of four-bit inputs and compare overflow flags against mathematical bounds.'),
('Shifts and Rotates','bitvectors','implemented','include/Weighstone/CCINT/BitVector.hpp','Logical shifts fill with zero; arithmetic right shift preserves the sign; rotates wrap moved bits around the specified width. CCINT totalizes oversized shifts explicitly.','Implement target-specific shift preconditions in the IR adapter rather than assuming CCINT matches LLVM or C semantics.','In a number of IRs a shift count greater than or equal to width yields poison or is otherwise invalid.','Test rotate-left and rotate-right on one-hot vectors for every shift count.'),
('Known Bits','analysis','implemented','include/Weighstone/CCINT/KnownBits.hpp','KnownBits carries masks for bits definitely zero and definitely one. A mask pair must not mark the same bit in both sets. Transfer operations implement AND, OR and XOR.','Use joins as intersection of facts from multiple control-flow predecessors; retain only facts shared by both.','The domain currently lacks backwards transfer functions, sophisticated arithmetic transfer and a CFG worklist.','Derive masks for (x & 15) given an unknown 8-bit x.'),
('Integer Intervals','analysis','implemented','include/Weighstone/CCINT/Interval.hpp','Intervals represent inclusive mathematical integer bounds; optional lower or upper bounds denote infinities, and bottom denotes an empty set. Operations overapproximate results.','Apply join at merges and meet when learning a branch guard.','Intervals do not retain correlations such as y=x+1, so some relational properties are unprovable.','Intersect [0,20] with [9,30] and multiply [-2,3] by [4,10].'),
('Congruence Domains','analysis','implemented','include/Weighstone/CCINT/Congruence.hpp','Congruence(r,m) denotes all x with x congruent to r modulo m. Modulus 0 denotes the exact singleton r; modulus 1 denotes top.','Apply meet via generalized CRT and join via gcd of strides and offsets.','Treat modulus zero as a singleton, not as an invalid division request.','Join even numbers with integers congruent to two modulo four.'),
('Interval-Congruence Product','analysis','implemented','include/Weighstone/CCINT/Abstract.hpp','AbstractInteger combines a range with a congruence, reduces each finite endpoint independently, and intersects singleton congruences even with unbounded intervals. Contradictions expose a bottom interval.','Use meet to combine range and modular facts; reduce after operations to remove impossible endpoint values.','This is not a complete reduced-product fixed-point engine.','Determine feasible values in [0,10] congruent to one modulo two.'),
('Widening and Fixed Points','analysis','implemented','include/Weighstone/CCINT/Interval.hpp','Interval::widen(next) preserves stable bounds and drops outward-moving bounds to infinity. Bottom is the identity for an initial widening. Exact scale preserves one-sided bounds and handles zero explicitly.','Call previous.widen(next) at selected loop headers in a caller-managed worklist. DirectedGraph supplies CFG traversal and strongly connected components.','A generic worklist solver and narrowing policy are not supplied. Widening is deliberately less precise than join.','Apply widening to successive ranges [0,0], [0,1], [0,2] and explain convergence.'),
('Integer Matrices','linear algebra','implemented','include/Weighstone/CCINT/Matrix.hpp','IntegerMatrix stores arbitrary-precision rectangular matrices and exposes dimensions and checked element access. Determinants use an exact fraction-free algorithm.','Keep matrix entries exact when deriving dependence or lattice facts.','Hermite normal form, Smith normal form and LLL are not implemented.','Check determinant of identity and zero matrices at several sizes.'),
('Bareiss Determinants','linear algebra','implemented','include/Weighstone/CCINT/Matrix.hpp','Bareiss elimination replaces naive rational Gaussian elimination with divisions known to be exact for integer matrices. Row swaps preserve the determinant up to sign.','Use this as a numerical core for stronger exact linear algebra in future releases.','This implementation is not designed as a high-performance sparse determinant solver.','Compare determinants against Leibniz expansion for all small 3x3 matrices.'),
('Linear Expressions','constraints','implemented','include/Weighstone/CCINT/Linear.hpp','LinearExpr stores a constant plus a sparse map of variable coefficients. Terms that cancel exactly are removed. Scaling preserves affinity and evaluation is partial for missing variables.','Represent induction offsets and array subscripts before handing them to more powerful solvers.','Variables are identified by strings; a compiler adapter should intern and scope identifiers.','Build and simplify 3*x+2*y-3*x+7.'),
('Linear Range Evaluation','constraints','implemented','include/Weighstone/CCINT/Linear.hpp','LinearExpr::range evaluates affine forms over independent variable intervals. Positive and negative coefficients are handled by interval multiplication.','Supply SSA range facts to obtain conservative expression bounds.','Correlations are discarded. For x-x, canonicalizing the expression before ranging can avoid top-level precision loss.','Bound 5*x-2*y+3 with x in [0,10] and y in [-3,4].'),
('Conservative Inequality Proofs','constraints','implemented','include/Weighstone/CCINT/Constraints.hpp','check supports <=, >= and equality against constants and returns proven, disproven or unknown. A proof arises only when the interval bound establishes the claim for every admissible input.','Treat disproven as the proposition being false for all members of the input range, not just a found counterexample.','Unknown is an ordinary valid result, not a solver crash.','Prove x<11 for x in [0,10] by encoding x<=10.'),
('Symbolic Expression Trees','symbolic','implemented','include/Weighstone/CCINT/Symbolic.hpp','Expr represents constants, named variables, addition, multiplication and unary negation via immutable shared nodes. String serialization makes debugging convenient.','Share immutable terms and avoid mutation after the compiler has cached facts about a symbol.','The current implementation does not hash-cons across independently built trees, nor encode divisions or bitvector semantics.','Construct two equivalent but differently grouped sums and inspect their strings.'),
('Rewriting and Simplification','symbolic','implemented','include/Weighstone/CCINT/Symbolic.hpp','The small simplifier folds constants, removes additive zeros and multiplicative identities, collapses double negation and recognizes direct x+(-x) cancellation.','Build every new rewrite with explicit semantic preconditions.','Algebraic simplification is intentionally incomplete: it does not generally reassociate or distribute.','Extend the simplifier to fold a pair of nested constants and property-test the rule.'),
('Equality Queries','proof','implemented','include/Weighstone/CCINT/Proof.hpp','prove_equal examines the simplified difference between two mathematical integer expressions. It can establish some equalities, otherwise it reports unknown.','Use it as a fast proof layer before invoking an external SMT or polynomial normalizer.','Unknown does not imply inequality. The API has no witness-producing disproof at present.','Check x-x=0, x+1=x and (x+0)=x and classify all three results.'),
('Affine Recurrences','loops','implemented','include/Weighstone/CCINT/Recurrence.hpp','AffineRecurrence describes x(n)=initial+step*n over nonnegative iteration indices. Exact arithmetic means long-running loops do not overflow in the model.','Use this form only after proving that an SSA induction value follows the same recurrence.','Wrapping machine recurrences require an explicit modular model; the current type assumes mathematical integers.','Compute the 10^9-th term of a recurrence with a 200-bit step.'),
('Trip Counts','loops','implemented','include/Weighstone/CCINT/Loop.hpp','AffineRecurrence supports strict and inclusive comparisons in both directions. An initially false guard has zero trips; a true guard with a nonterminating step returns nullopt. LoopRange models terminating signed-stride half-open loops and iteration-count tiling.','LoopNest builds an ordinal-coordinate Polyhedron and an AffineMap from ordinals to induction values. Tile sizes are positive iteration counts, independent of induction stride.','This API does not model arbitrary loop exits, side effects or nonlinear updates. Numeric tiling is not an automatic legal loop-reordering proof.','Tile initial=10, stop=-2, step=-3 into groups of three and check the partial final tile.'),
('Induction Variable Recognition','loops','planned','manual/chapters/29-induction-variable-recognition.md','A real compiler must discover recurrences in SSA phi nodes, match incoming loop edges, and prove dominance and update properties.','Create an adapter over your compiler IR which maps candidate phis to AffineRecurrence only when constraints are discharged.','No SSA analysis or CFG implementation is shipped in the header collection.','Design detection for i=phi(0,i+2), guarding against irreducible control flow.'),
('Bitwidth Reduction','optimization','planned','manual/chapters/30-bitwidth-reduction.md','Value ranges and known-bit masks can prove when a value fits a narrower representation. Width changes must preserve all uses, comparison semantics and potential overflow properties.','Compute signed/unsigned minimal widths from proved bounds and use target-specific cast operations.','The current library provides primitives, not an end-to-end IR rewrite.','For values in [0,1000], determine the minimum unsigned bit width.'),
('Bounds-Check Elimination','optimization','implemented','include/Weighstone/CCINT/Tensor.hpp','prove_in_bounds(domain, access, shape) checks every affine index against exact nonnegative tensor extents. Only proven authorizes elimination of the corresponding logical bounds check. Mixed validity can return unknown.','Combine Polyhedron iteration facts with an AffineMap of logical subscripts. TensorLayout::linearize translates them to a single affine element offset.','No IR checks are removed by this library. Logical bounds do not establish physical allocation size, nonaliasing or machine-integer no-overflow facts.','Prove accesses A[2*i+1] safe given i in [0,9] and length=20.'),
('Strength Reduction','optimization','planned','manual/chapters/32-strength-reduction.md','Strength reduction replaces expensive arithmetic with equivalent cheaper operations, such as multiplication by an integer constant using shifts/additions.','Use a cost model plus signedness and overflow contracts to decide whether a rewrite is both legal and profitable.','Modern instruction costs are target-dependent; arithmetic equivalence alone is insufficient.','Compare x*10 with (x<<3)+(x<<1) under wrapping 32-bit arithmetic.'),
('Dependence Analysis','polyhedral','implemented','include/Weighstone/CCINT/Loop.hpp','analyze_dependences enumerates bounded integer iteration domains and ordered affine memory accesses, retaining every conflicting flow, anti and output pair. The complete DependenceGraph supports loop permutation legality and loop-carried dependence queries.','Use increasing ordinal coordinates and supply proven nonaliasing memory identities. Preserve access-list order within each iteration. A failed or budget-exhausted analysis returns unknown and no graph.','This bounded analyzer does not infer aliases, handle conditional accesses, synthesize schedules or model non-memory effects. A complete graph is a precondition for legality checks.','Model A[i]=A[i-1]+1 and show why the outer loop cannot run in parallel.'),
('ISL and Presburger Backends','polyhedral','planned','include/Weighstone/CCINT/Polyhedron.hpp','The implemented Polyhedron provides conjunctions of integer affine constraints, intersection, membership, unary bounding boxes and budgeted proofs. Integer-strengthened Fourier-Motzkin elimination proves emptiness; only witnesses or complete enumeration establish nonemptiness. External ISL and complete Presburger backends remain future work.','Use the local exact APIs for bounded and simple relational problems; map unsupported queries to unknown. Future foreign solvers must preserve context lifetime and distinguish timeout from disproof.','No ISL adapter, projection, union representation or parametric scheduler ships in this version. A rational solution is not proof of integer feasibility.','Compare 2*x+2*y=1 with x-y>=1 and explain an emptiness proof versus an inconclusive unbounded search.'),
('SMT and Bitvector Solving','solver','planned','manual/chapters/35-smt-and-bitvector-solving.md','SMT solvers can reason about bitvector equivalence, signed versus unsigned comparisons and quantified constraints beyond simple interval proofs.','Design a backend-neutral query containing assumptions, theory, time budget and expected proof kind.','Timeout and unknown are not disproof; solver formulas must accurately model IR poison/traps.','Encode the failure of unsigned 8-bit x+1>x at x=255.'),
('Testing and Differential Fuzzing','quality','implemented','tests/test_analysis.cpp','Tests cover low-width arithmetic, reduced-product soundness, bounded integer-polyhedron oracles, tensor layouts, signed-loop tiling and every directed graph on three vertices. Every header compiles independently.','Use independent enumeration or mathematical identities as oracles, and exercise budget exhaustion and empty/zero-dimensional domains.','These finite checks are not formal verification or a complete fuzzing campaign.','Add a correlated integer constraint system and compare every positive proof with enumerated points.'),
('Undefined and Poison Semantics','semantics','planned','manual/chapters/37-undefined-and-poison-semantics.md','Compiler IRs may distinguish wrapping, undefined overflow, poison-producing arithmetic, saturating operations and trapping conversions. These distinctions are essential during proof and rewrite.','Attach a semantic policy to each IR operation and reject transformations when assumptions are incomplete.','Treating all arithmetic as modular or all arithmetic as mathematical is unsound for a general compiler.','Write a table comparing add behavior in C, LLVM, and a modulo arithmetic DSL.'),
('Compiler IR Integration','integration','planned','manual/chapters/38-compiler-ir-integration.md','A compiler-specific adapter should map SSA values, constants, types, branches and loop structure to CCINT facts, and invalidate cached facts when IR changes.','Keep core headers independent of LLVM, GCC and custom compiler ownership types.','The source distribution deliberately does not include an LLVM pass or an SSA bridge.','Design an abstract ValueRef and AssumptionSet to mediate a range query.'),
('Caching and Resource Budgets','performance','planned','manual/chapters/39-caching-and-resource-budgets.md','Demand-driven analysis avoids wasting time on every integer expression. Symbolic DAGs, solver results and normalized constraints can be memoized under the right assumptions.','Key caches by target semantics, expression identity, assumption scope and dataflow epoch.','Stale facts and unbounded solver effort can cause miscompilation or unacceptable build times.','Specify a cache invalidation scheme for loop unrolling and SSA replacement.'),
('Roadmap and Extension Contracts','roadmap','implemented','README.md','Weighstone includes exact arithmetic, integer-polyhedron proofs, bounded dependence analysis, tensor metadata, graph utilities and loop tiling. Advanced IR integration, parametric scheduling, SMT, proof certificates and generic fixed-point engines remain follow-up work.','Preserve conservative result types and test invariants before adding optimization features.','A header existing in the file tree does not imply the entire associated research topic is implemented.','Prioritize either a Presburger adapter or a fixed-point engine and define acceptance tests.'),
]
assert len(TOPICS)==40

DETAILS = {
18: '''## Graph and widening APIs

`support/Graph.hpp` implements `weighstone::support::DirectedGraph`. Vertex IDs
are stable dense indices. Duplicate edges are ignored. `topological_order()`
returns an optional vector (nullopt on cycles); `strongly_connected_components()`
uses iterative Kosaraju traversal with sorted members and topologically ordered
components. `reachable(a,b)` includes the zero-edge path. Traversals are
deterministic. SCC traversal is O(V+E), plus sorting component members;
topological sorting uses a minimum-ID priority queue.

`Interval::widen(next)` uses the receiver as the old state. For example,
`Interval(0,3).widen(Interval(0,4))` becomes `[0,+infinity)`. Callers own the
worklist, update order, widening placement, convergence checks and IR facts.
''',
19: '''## Tensor metadata and layouts

`include/Weighstone/CCINT/Tensor.hpp` supplies `TensorShape` and `TensorLayout`.
Extents, strides, element counts and offsets are BigInt values. Rank zero is a
scalar with one element; any zero extent makes a tensor empty. Negative extents
and rank mismatches throw. Broadcasting aligns trailing axes: equal extents or
an extent of one are compatible, including the 0-with-1 result of zero.

`TensorLayout::row_major(shape, base)` constructs contiguous strides.
`offset(indices)` validates logical bounds. `storage_range()` returns the exact
inclusive hull of addressed offsets, or bottom for an empty tensor; it does not
assert that every offset in the hull is addressed. Signed and zero strides are
valid. `permute(order)` uses order[new_axis]=old_axis. `broadcast_to(shape)`
introduces zero strides; writes through such views can alias. `reshape(shape)`
requires equal element count and row-major contiguity; singleton-axis strides
are irrelevant. `linearize(access)` returns a one-output affine element map.

For example, row-major shape `{2,3}` has strides `{3,1}` and offset `{1,2}` = 5.
Permuting axes `{1,0}` gives shape `{3,2}`, strides `{1,3}`, and offset `{2,1}` = 5.
No storage is allocated, and allocation bounds and byte-size conversions remain
caller responsibilities.
''',
27: '''## Signed loops and tiling

`LoopRange(10,-2,-3)` visits 10, 7, 4, 1. `tile_count(3)` is two; `tile(0,3)`
visits 10, 7, 4 and `tile(1,3)` visits 1. Empty loops have zero tiles. Invalid
ordinals throw out_of_range; zero steps and nonpositive tile sizes are invalid.
`LoopNest({"t"}, {{10,-2,-3}})` describes ordinals 0 through 3 and maps them to
10-3*t using `induction_values()`. This separation prevents descending source
loops from being confused with ascending lexicographic execution order.
''',
33: '''## Memory-dependence contracts

`AffineAccess` contains an object identity, affine subscript map and write flag.
Access-list order is execution order within each point; domain variable order
defines increasing lexicographic point order. Distinct nonempty object names
must be known nonaliasing. Equal names require the same element-coordinate
system; flatten views with `TensorLayout::linearize` when necessary. Empty
identities return unknown. Read/read pairs do not induce ordering edges.

`DependenceAnalysis::complete` describes completeness, not absence of edges.
Its optional graph is present only for complete analysis. `DependenceGraph`
stores target-minus-source iteration distances. Unknown distances make legality
unknown unless another known edge already disproves it. `permutation_legal`
requires lexicographically nonnegative transformed distances and retains
within-iteration statement order. `parallel_axis` checks for dependencies
carried by that axis at a fixed outer prefix. Non-memory ordering must be added
by the caller. Neither query alone proves arbitrary compiler IR rewrites safe.

The algorithm enumerates candidate box points and then event pairs, with defaults
of 100,000 points and 1,000,000 pairs. Work is O(E^2) pair checks for E events,
plus exact affine evaluation, graph insertion and coefficient arithmetic. Graphs
can retain multiple distance facts for the same source/target statement pair.
''',
34: '''## Implemented local polyhedron API

`Polyhedron({"i","j"})` declares an ordered coordinate space. `add` validates
variables; `intersect` requires matching spaces. `contains` requires every
coordinate. `bounding_box` derives only unary bounds and is an overapproximation.
`integer_points` enumerates the finite box and filters constraints; its complete
verdict must be proven before treating the returned vector as exhaustive.
The zero-dimensional universe has one empty tuple.

`is_empty` returns disproven only with an integer witness. Elimination combines
oppositely signed inequalities and divides coefficient GCDs, rounding bounds
down for integer coordinates. Contradictions prove emptiness, but failure to
derive a contradiction is not proof of satisfiability. `prove(constraint)` checks
universal truth using interval bounds and emptiness of negated integer branches.
Disproven means false throughout the domain, not one violating point. Mixed
domains and incomplete analyses return unknown. Empty domains prove universal
properties vacuously.

`AnalysisBudget` caps candidate box points (100,000) and intermediate elimination
inequalities (4,096). Each proof branch gets that budget. Bounds do not limit
input length, BigInt bit size, total memory or wall-clock time. Exhausted queries
stay unknown. `AffineMap` validates declared inputs, evaluates ordered outputs
and composes positionally with `outer.compose(inner)`.
''',
}

def slug(s):
    return re.sub(r'[^a-z0-9]+','-',s.lower()).strip('-')

chapters=[]
for i,(title,domain,status,path,overview,method,pitfall,exercise) in enumerate(TOPICS,1):
    file=f'manual/chapters/{i:02d}-{slug(title)}.md'
    chapters.append((i,title,file,status))
    payload=f'''# Chapter {i}: {title}

**Domain:** {domain} · **Status:** {status} · **Primary reference:** `{path}`

## Problem and model

{overview}

At every compiler optimization boundary it is necessary to identify what is known about the input program, which arithmetic universe is being modeled, and whether the facts have been inferred conservatively. A result is useful only insofar as it remains true for every execution admitted by those assumptions. This chapter describes this issue in the context of **{title.lower()}**.

## Method and API contract

{method}

When implementing or extending the API, document preconditions, output guarantees, exceptional situations, and complexity. Use exact integer values for mathematical reasoning; when the source language describes fixed-width behavior, perform arithmetic through `BitVector` or a formally specified adapter. A query should return an explicit `unknown` if it lacks enough evidence, rather than guessing from an example.

## Worked reasoning pattern

Start with an IR expression and annotate its width, signed interpretation, reachable input states, and relevant control-flow guards. Translate the provable properties into the smallest appropriate representation: `BigInt` for exact constants, `Interval` for independent bounds, `Congruence` for modular information, and `LinearExpr` for affine sums. Apply only operations with matching semantics. Check the result against a boundary case, then decide whether it is strong enough to justify a rewrite.

For a concrete experiment, begin by constructing a test case in `tests/test_ccint.cpp`. Use the public umbrella include `#include <Weighstone/CCINT.hpp>`, and build the test using the project CMake target. If the technique described in this chapter is marked **planned**, this experiment is a design exercise, not a claim that its high-level optimization is already shipped.

## Failure modes and precision

{pitfall}

There are two distinct outcomes when an optimizer cannot establish a result: the proposition may actually be false, or the chosen abstraction may simply be too weak to prove it. Reserve `disproven` for the former when a sound proof is available, and `unknown` for the latter. This distinction is especially important when eliminating safety checks.

## Exercise

{exercise} Add a regression test that covers a normal case, an extreme boundary, and at least one invalid or inconclusive case. Explain why your assertions use mathematical versus bitvector semantics.

## Further engineering

The project `MANIFEST.yaml` records this chapter and the corresponding header or design reference. For a production optimizer, follow up with property-based testing, compile-time budgets, diagnostics for rejected rewrites, and explicit review of the mathematical assumptions. Do not attach target-specific behavior to a domain-independent data structure unless its semantics are specified.
'''
    if i in DETAILS:
        payload = payload.replace('## Further engineering', DETAILS[i] + '\n## Further engineering')
    (ROOT/file).write_text(payload,encoding='utf8')

index='''# CCINT: A Compiler-Centric Integer Toolkit

**Weighstone Manual — 40 Chapters**

This manual describes implemented APIs, mathematical invariants, known limitations, and designs for future compiler components. Chapters with `planned` status are explanatory design material, **not** implemented features. All current Weighstone headers use C++20 and Boost.Multiprecision. Start with the `README.md` and build the test executable before adding an optimizer pass.

## Table of contents

'''
for i,title,file,status in chapters:
    index+=f'{i}. [{title}](chapters/{Path(file).name}) — {status}\n'
index+='''
## Conventions

All mathematical integer inequalities denote properties of arbitrary-precision `BigInt` values. Signed and unsigned machine integers require explicit `BitVector` width. Intervals have inclusive endpoints; a missing endpoint denotes infinity and bottom denotes emptiness. `Verdict::unknown` means insufficient proof, not falsity. Do not apply `Expr` algebraic laws to a compiler IR with poison, traps or nonmodular overflow unless the relevant conditions are established.

## Quality and scope

The included tests cover representative correctness cases. They are not a full formal verification or performance study. Extend low-bitwidth enumeration, metamorphic tests and differential comparison against independent numerical libraries before using Weighstone to justify safety-critical transformations.
'''
(ROOT/'manual/CCINT.md').write_text(index,encoding='utf8')

# Create a file-by-file manifest with truthful status and purpose.
PURPOSE={
'README.md':'Project introduction, compile instructions, implemented scope and limitations',
'LICENSE':'MIT distribution terms',
'CMakeLists.txt':'CMake interface library, example target and CTest regression executable',
'.gitignore':'Ignore local build and cache outputs',
'.github/workflows/ci.yml':'Continuous integration compilation and CTest execution',
'support/Config.hpp':'Common semantic version and input assertion utility',
'support/Hash.hpp':'Common deterministic in-process hash combining helper',
'support/Result.hpp':'Reusable tri-state proof verdict and value/error result',
'support/Graph.hpp':'Deterministic directed graphs, reachability, topological ordering and iterative SCCs',
'support/Permutation.hpp':'Shared rank and permutation validation',
'include/Weighstone/CCINT.hpp':'Umbrella include exposing all implemented CCINT headers',
'include/Weighstone/CCINT/BigInt.hpp':'Boost-based exact integer alias, integer division and GCD/LCM operations',
'include/Weighstone/CCINT/Rational.hpp':'Normalized exact rational arithmetic',
'include/Weighstone/CCINT/NumberTheory.hpp':'Extended GCD, modular inverse, generalized CRT and divisibility',
'include/Weighstone/CCINT/BitVector.hpp':'Fixed-width modular integers, signed interpretation, shifts and rotates',
'include/Weighstone/CCINT/Checked.hpp':'Signed/unsigned overflow-aware addition, subtraction and multiplication',
'include/Weighstone/CCINT/KnownBits.hpp':'Definite-zero and definite-one bitmask analysis operations',
'include/Weighstone/CCINT/Interval.hpp':'Unbounded/bounded intervals with lattice and arithmetic operators',
'include/Weighstone/CCINT/Congruence.hpp':'Modular-congruence lattice operations and meet using CRT',
'include/Weighstone/CCINT/Abstract.hpp':'Interval-congruence product with one-sided, singleton and bottom reduction',
'include/Weighstone/CCINT/Matrix.hpp':'Exact integer matrix determinant via Bareiss elimination',
'include/Weighstone/CCINT/Linear.hpp':'Sparse affine expressions and interval range evaluation',
'include/Weighstone/CCINT/Constraints.hpp':'Conservative proof of affine constraints from interval bounds',
'include/Weighstone/CCINT/Recurrence.hpp':'Mathematical affine induction recurrence and strict/inclusive signed-direction trip counts',
'include/Weighstone/CCINT/Symbolic.hpp':'Immutable expression trees and small algebraic normalizer',
'include/Weighstone/CCINT/Proof.hpp':'Conservative mathematical integer equality query',
'include/Weighstone/CCINT/Polyhedron.hpp':'Integer affine sets, budgeted elimination, enumeration and conservative constraint proofs',
'include/Weighstone/CCINT/Affine.hpp':'Validated ordered affine maps, evaluation and positional composition',
'include/Weighstone/CCINT/Tensor.hpp':'Exact tensor shapes, strided layouts, broadcasting, reshape and affine index bounds',
'include/Weighstone/CCINT/Graph.hpp':'Dependence graphs with loop permutation and parallel-axis legality queries',
'include/Weighstone/CCINT/Loop.hpp':'Signed counted loops, tiling, ordinal nests and bounded affine memory dependences',
'examples/basic.cpp':'Standalone range and machine-integer usage example',
'examples/loop_tensor.cpp':'Runnable tensor bounds, dependence legality and partial loop tiling example',
'tests/test_ccint.cpp':'Compiler-built smoke and boundary regression suite',
'tests/test_properties.cpp':'Exhaustive small-width bitvector and division identity property tests',
'tests/test_analysis.cpp':'Core hardening regressions and independently enumerated integer-polyhedron proof checks',
'tests/test_optimization.cpp':'Tensor layout, exhaustive small graph, signed tiling and memory dependence regressions',
'manual/CCINT.md':'CCINT 40-chapter manual index, conventions and scope',
'tools/build_manual.py':'Deterministically regenerate all forty manual chapters and YAML manifest',
'MANIFEST.yaml':'File-by-file repository inventory, purpose and implementation status',
'AGENTS.md':'Implementation and review instructions for automated and human contributors',
}
for i,title,file,status in chapters:
    PURPOSE[file]=f'Manual chapter {i}: {title}; capability {status}'

lines=['schema: weighstone.manifest.v1','project: Weighstone','component: CCINT','version: 0.1.0','language: C++20','distribution: header-only','requires:','  - Boost.Multiprecision headers','optional_backends_integrated: []','files:']
for path in sorted(PURPOSE):
    descr=PURPOSE[path].replace("'","''")
    status='planned-design' if path in [c[2] for c in chapters if c[3]=='planned'] else 'implemented-or-documentation'
    if path == 'MANIFEST.yaml': status='generated-inventory'
    lines.extend([f"  - path: '{path}'",f"    purpose: '{descr}'",f"    status: {status}"])
(ROOT/'MANIFEST.yaml').write_text('\n'.join(lines)+'\n',encoding='utf8')
print('Generated chapters:',len(chapters),'manifest files:',len(PURPOSE))
