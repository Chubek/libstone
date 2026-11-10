# Satie SAT/SMT and Equinox-NG

The intended vendor layout is:

```
libstone/
  third_party/
    satie/
      include/SatieCDCL.hpp
      include/SatieLIA.hpp
    equinox-ng/
      EquinoxNG.hpp
  wetstone/
    integration/Satie.hpp
    integration/EquinoxNG.hpp
    symbolic-exec/Solver.hpp
```

The GitHub repository includes instructions for this vendor layout but does not check out these dependencies in tracked `master` tree. `wetstone/CMakeLists.txt` automatically adds the vendor headers if found.

## Boolean SAT

`symbolic::SatieSolver` uses `satie::CNF` and `satie::solve_cdcl`, with `SolveResult::satisfiable/unsatisfiable` and its assignment. The translation is a Tseitin CNF encoding of Boolean variables, AND, OR, NOT, Boolean equality and disequality. Unsupported theory operations return `Unknown`; they are **not** replaced with unconstrained propositional atoms. With absent Satie headers it returns `Unknown` rather than inventing an answer.

## Linear integer arithmetic

`integration::check_linear_integer` translates named variables and `sum(a_i*x_i)<=b` into `satie::lia::LIASolver`. The width parameter is a finite bit-blasting bound. Satie returns `UNKNOWN`, not `UNSAT`, when a model may exist beyond that bound. Wetstone forwards the distinction.

## SMT-LIB2

`symbolic::smtlib2` renders declarations, assertions, and `(check-sat)`. It is intended for exporting path conditions to an external SMT-LIB2-compliant solver. The internal Boolean/Satie adapter is not a general first-order SMT solver.

## Equality saturation

`integration::saturate` takes `equinoxng::Term`, a vector of `equinoxng::RewriteRule`, and `equinoxng::RunnerConfig`. It builds an e-graph, runs the rewrite rules and extracts a minimum-cost term using upstream Equinox-NG. This is an **adapter**, not a second e-graph implementation. When Equinox-NG is absent the adapter's types/functions are not exposed; `WETSTONE_HAS_EQUINOX_NG` is false.

## Verified integration status

The public upstream Satie and Equinox-NG source APIs were inspected on GitHub. Actual vendored builds were not possible in the isolated generation environment. Real-world integration requires testing your specific revisions, particularly because the LibStone README warns of API mismatches in extended Equinox-NG usages elsewhere in the repository.
