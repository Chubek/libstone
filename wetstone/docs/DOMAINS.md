# Abstract-domain catalog

| Header | Domain | State representation | Trade-off |
|---|---|---|---|
| `Constant.hpp` | Constant propagation | bottom, value, top | Fast and cheap |
| `Interval.hpp` | Interval | `[lo, hi]`, bottom | Non-relational numeric bounds |
| `Sign.hpp` | Sign | 3-bit negative/zero/positive mask | Fast branch/numeric properties |
| `Parity.hpp` | Parity | even/odd bits | Congruence mod 2 |
| `Congruence.hpp` | Integer congruence | `modulus, residue` | Divisibility and CRT |
| `KnownBits.hpp` | Known bits | zero/one bit masks | Bitwise optimizations |
| `BitvectorInterval.hpp` | Unsigned fixed-width interval | bound pair plus width | Wrap handled by Top; full arithmetic + comparisons |
| `TypedInt.hpp` | Typed integer reduced product | BitvectorInterval ⊗ KnownBits | Range + per-bit precision, constant cross-propagation |
| `StridedInterval.hpp` | Strided intervals | interval + congruence | Sparse ranges |
| `FiniteSet.hpp` | Finite sets | at most N values, else Top | Exact small enumerations |
| `DBM.hpp` | Difference bounds | NxN matrix | Relational `x-y<=c` |
| `Octagon.hpp` | Octagon | 2N signed difference nodes | `±x±y<=c` constraints |
| `AffineEqualities.hpp` | Copy-offset equality | `x=y+c` facts | Restricted linear identities |
| `Predicate.hpp` | Boolean predicates | finite tri-state map | Path conditions |
| `Product.hpp` | Cartesian product | pair of two domains | Independent facts |
| `ReducedProduct.hpp` | Interval + congruence | reduced pair | Cross-domain narrowing |
| `Disjunctive.hpp` | Bounded disjunctions | vector of domain elements | Path-sensitive precision |
| `PartitionedEnvironment.hpp` | Trace partitions | bounded map + overflow summary | Partition-aware precision |

## Precision ordering

`a.leq(b)` means `a` describes a subset of the values that `b` describes. `join` overapproximates union. `meet` overapproximates intersection; implementations that cannot represent exact intersection may return a weaker meet. `widen` must overapproximate the new state to accelerate convergence.

The congruence lattice uses modulus zero for exact singletons, modulus one for Top, and an explicit Bottom. CRT combination uses 128-bit intermediates and falls back to a sound overapproximation if the resulting modulus exceeds signed 64-bit range.

`DBM` runs Floyd–Warshall closure for inserted difference constraints. `Octagon` uses a doubled DBM and enforces paired sign constraints, then applies **strong closure** (Miné): the unary `2x<=c` / `-2x<=c` entries are rounded down to the nearest even bound (since `2x` is always even) and the matrix is re-closed to a fixpoint, recovering precision the plain DBM closure misses. Strong closure runs after `constrain`, `join`, and `meet` but is deliberately skipped after `widen` so widening convergence is not disturbed. A `bound(x,sx,y,sy)` query reads the tightly closed entry.

`KnownBits` requires equal widths for binary operations. `BitvectorInterval` is unsigned and now carries complete arithmetic (`add`/`sub`/`mul`/`div`/`mod`/`shl`/`shr`/`neg`) plus bitwise and comparison transfer functions; any operation that may wrap conservatively degenerates to `top` rather than modeling a disjunctive result. `TypedInt` is a reduced product of `BitvectorInterval` and `KnownBits`: arithmetic delegates to the range (dropping bit precision) and bitwise delegates to bits (recovering a singleton range when bits become fully determined); `reduce()` propagates constants between the two and turns range/bits contradictions into bottom. Signed comparison and wrap-aware disjunctive intervals remain future work.
