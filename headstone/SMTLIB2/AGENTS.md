# Completing Headstone SMTLIB2

This directory is an independent C++17 header-only SMT-LIB 2 library. The
checked-in scaffold is **not SMT-LIB conformant and not a production solver**.
Read the `@implstring` doc comments in each header as implementation contracts.
Keep this file and those comments synchronized with actual behavior and tests.
Do not describe an interface, theory or logic as implemented until its entire
execution path has been validated.

## Non-negotiable boundaries

- Consumers need only `include/smtlib2/` and the C++17 standard library. Avoid
  runtime subprocesses, mandatory link dependencies, repository-relative
  includes, and direct use of `third_party/satie` or its code. Satie's design
  may be studied as inspiration; do not copy implementation without review.
- Amalgam8 is a build-time generator. The current `.am8` bundles Firestone's
  BigInt, and `theory.hpp` exposes it. Keep bundled license notices. Review and
  test each additional Firestone or other Pebble dependency before bundling;
  Amalgam8's dependency catalog and bundle eligibility govern what is feasible.
  Firestone LibDSL powers Amalgam8's frontend; investigate `liblr`, `libtype`,
  `libsema`, `libir`, `librewrite`, and `libbignum` for suitable development
  facilities, without making the consumer depend on the repository. Avoid
  duplicating or redefining Firestone namespaces when mixing generated bundles.
- Do not silently produce `sat` or `unsat` for unsupported features. `unknown`
  must remain distinct from errors, and SAT answers require a model consistent
  with all assertions. Resource exhaustion, unsupported theory combinations,
  and incomplete proof searches must never masquerade as `unsat`.
- Preserve C++17 compatibility, header-only ODR safety across translation
  units, per-session ownership, deterministic results, and useful source spans.
  Do not create global mutable solver state. Validate before mutating a command.

## Present behavior and known gaps

SMT-LIB 2.6 is the target specification for syntax and protocol. This is a
compatibility target, not a conformance claim; see `README.md` for supported
behavior.

The lexer validates token categories and carries byte-based spans; delimited
text preserves non-ASCII bytes without validating the encoding. `ast.hpp`
provides typed syntax separately from the
command executor. `elaboration.hpp` normalizes scoped Boolean terms,
including simultaneous `let` and nonrecursive Boolean definitions.
`solver.hpp` provides bounded DPLL with model validation and a separate
truth-table reference. The session supports an incremental assertion stack,
and reuses active Boolean encodings, not search state. The bounded `QF_BV`
backend checks a subset of 1–64-bit operations by exhaustive search over up
to 16 referenced bits; see README for limits. Exact integer/rational value
utilities are not theory solvers. The stream driver can emit wire errors
and resume at safe command boundaries. Full protocol conformance, proof
production, and non-Boolean theory solving remain incomplete. `SMTLIB2.xbind` is an empty
placeholder, not functioning bindings.

The umbrella can be compiled as a C++17 CLI (`SMTLIB2_CLI`) or stream REPL
(`SMTLIB2_REPL`); set one flag in one translation unit only. Both use the
reviewed Firestone OptParser extraction; the interactive directives use
LibDSL Directives. Preserve the macro-free include path and clean-install
consumer tests. Do not describe this extraction as a whole-LibDSL bundle.

## Implementation sequence

1. Document deviations from SMT-LIB 2.6. Extend the
   lexer for every token category and valid character, including numerals,
   decimals, bit-vector literals, keywords, strings and quoted symbols. Test
   boundary lengths, comments, malformed input, diagnostics and byte offsets.
2. Implement syntax variants for commands, sorted terms, constants, attributes,
   qualified/indexed identifiers, sorts, function declarations, annotations,
   `let`, quantifiers and recursive definitions. Distinguish syntax errors,
   semantic errors and unsupported features; parse and print round-trip suites.
3. Build elaboration and type checking: canonical symbol identity, scope and
   shadowing, simultaneous `let`, sort arity, overloaded/indexed operators,
   logic selection and defined functions. Use immutable term IDs and a scoped
   symbol environment; do not expose stale references after pop/reset.
4. Implement command processing per the pinned standard: options, `set-info`,
   declarations/definitions, incremental stack, global declarations, all reset
   behavior, check-sat-assuming, model/value/core/proof queries, errors and
   exit. Check all preconditions before committing state. Replace provisional
   response strings with typed responses and standard rendering; test failed
   command atomicity and previous-result invalidation. Make solver/model/proof
   production options meaningful.
5. Implement a real incremental SAT backend and CNF/Tseitin transformation.
   Preserve the reference Boolean enumerator for small differential tests.
   Add SAT model evaluation and checkable UNSAT proof or certificate support.
   Introduce DPLL(T) and a precise theory plugin interface with explanations,
   backtracking, model construction and explicit incompleteness.
6. Progress incrementally through equality/uninterpreted functions, exact
   integers/rationals and linear arithmetic, bit-vectors, arrays, datatypes,
   floating point, strings, and quantifiers. Define supported logic fragments
   and solver completeness individually; finite-width bit-vectors require
   rigorous width, division-by-zero and signedness semantics. Firestone BigInt
   is available but is not itself a theory solver. Avoid host floating point
   for exact real semantics. Never conflate unspecified and undefined values.
7. Add a high-level C++ API for building typed terms and scripts, a safe
   extension surface, optional serialization, and an optional protocol driver.
   Implement bindings only after establishing an actual xbind contract; do not
   imply the existing empty file provides bindings. Ensure builds have no
   hidden nonstandard includes or generated build-time prerequisites.
8. Add normative and adversarial suites, fuzz lexer/parser, verify bounded
   depth and memory, test malformed state transitions, model validation and
   cross-check supported fragments against independent solvers. Test standalone
   installation in a clean directory, multi-TU inclusion, multiple compilers,
   sanitizers, and generated snapshot reproducibility. Third-party solvers may
   be optional test oracles, never a runtime dependency.

## Build and regeneration

Run the commands in `README.md`, including CTest and the Amalgam8 snapshot
comparison. To intentionally update the generated snapshot, run Amalgam8 with
`--api --bundle --include-root <libstone-root>` on a temporary copy of
`amalgam8/Firestone.am8`, inspect the generated header and license, then replace
`include/smtlib2/generated_firestone.hpp`. Run all tests and a clean install
consumer test. Generated material must remain hermetic and reproducible.
The same target verifies `generated_frontend.hpp` via the development-only
`amalgam8/extract_frontend.py` generator. Review its selected source ranges,
C++17 substitutions and license bytes before updating either pinned hash.
