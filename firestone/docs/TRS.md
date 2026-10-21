# Term, string, and graph rewriting

`Firestone::trs` is a header-only C++17 component. Include individual headers
from `<libtrs/...>` or use `<Firestone.hpp>`. Its API lives in `firestone::trs`.

`Term.hpp` and `TermRewriting.hpp` re-export the `firestone::rewrite` term,
parser, pattern matching, and `Rewriter` API; these are the **same types**, so
existing rules and terms can be used without conversion. Patterns bind named
leaf operators such as `?x`. A repeated variable must match the same term;
replacement variables must occur in the left-hand side. Rule order determines
which applicable rule fires first. Term rewriting supports top-down/bottom-up
passes, guards, and iteration/application limits.

`StringRewriter` applies the leftmost nonidentity substring replacement at each
step. In a tie, the first rule added wins. Empty left-hand sides are rejected;
empty right-hand sides delete text. `run(input, iteration_limit, application_limit)`
bounds both the number of replacement passes and the number of applications,
so nonterminating rule sets cannot loop forever. `run()` returns the resulting
string, stop reason, application count and ordered rule names. A graph already
in normal form reports `NormalForm` even when a limit is configured.

`Graph` is an append-only, hash-consed term **DAG**, not a general cyclic graph.
`add(op, children)` requires existing child IDs and returns an existing ID for
an identical operator/child tuple. `from_term()` and `to_term()` convert between
trees and graphs. `GraphRewriter` matches and rewrites graph nodes with named
leaf variables. Repeated variables refer to the same canonical subgraph;
substitution reuses bound node IDs. Each pass visits every *shared* input node
once, in top-down or bottom-up order. It produces a new root ID: existing roots
remain valid and unchanged. Guards receive a `const Graph&` and a map from
variable names to graph IDs. Limits apply to successful applications and
passes, not allocated graph nodes. The graph retains intermediate/unreachable
nodes until destroyed. `GraphReducer` provides the same operation for a
collection of rules.

```cpp
#include <libtrs/GraphCombinators.hpp>

firestone::trs::Graph graph;
auto x = graph.add("x");
auto i = firestone::trs::combinator(graph, "I");
auto expression = firestone::trs::apply(graph, i, x);
auto result = firestone::trs::reduce_combinators(graph, expression);
// result.root == x; expression is still available in graph.
```

`combinator_reducer()` supplies only the untyped `I`, `K`, and `S` rules over
binary `app` nodes. No other algebraic rules are implicit. `S` shares its
argument rather than cloning the bound subgraph. As with any untyped rewrite
system, reduction may not terminate: inspect `result.stop` and set appropriate
iteration/application limits. All three rewriters return `NormalForm` if a
pass finds no change, and a limit stop otherwise.
