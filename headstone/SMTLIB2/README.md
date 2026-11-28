# SMTLIB2

A C++17 header-only **foundation**, not a complete SMT-LIB implementation or a
production solver. Include `<smtlib2/smtlib2.hpp>` and link no library. The
headers and their bundled Firestone BigInt snapshot require only the C++
standard library; neither Libstone nor Satie is required by a consumer.
The syntax and protocol target [SMT-LIB 2.6](https://smt-lib.org/language.shtml);
the implementation supports only the subset described below.

```cpp
#include <smtlib2/smtlib2.hpp>
#include <iostream>

int main() {
    smtlib2::Session session;
    for (const auto& r : session.execute(
        "(declare-const p Bool) (assert (not p)) (check-sat) (get-model)"))
        std::cout << r.text << '\n';
}
```

The bounded S-expression reader validates SMT-LIB 2.6 token categories,
preserves token spelling and byte-based source locations, and normalizes
whitespace when printing. Strings and quoted symbols preserve non-ASCII
bytes without validating or normalizing their encoding. The parser itself
does not recover from errors.
`parse_script` provides typed syntax separately from execution; its printer
uses retained source tokens, not edits to typed nodes.
By default, and under `QF_UF` or `ALL`, execution is limited to Boolean terms.
The Boolean solver translates `not`, `and`,
`or`, `xor`, `=>`, `=`, `distinct`, and `ite` to CNF and runs bounded,
deterministic DPLL. It validates SAT models independently, and returns
`unknown` when resource limits are exhausted. `ReferenceBoolSolver` retains
the exhaustive reference engine. `Session` reuses active assertion encodings
across Boolean checks and assertion-only scopes, and rebuilds lazily when
declarations change. Temporary assumptions do not persist. Each check starts
a fresh DPLL search.

Use `(set-logic QF_BV)` to select the bounded bit-vector backend. For example,
`(declare-const x (_ BitVec 4)) (assert (= (bvadd x #b0001) #b0000))`
forces `x` to `#b1111`. This fragment supports `Bool` and 1–64-bit constants,
binary/hex literals, `bvnot`, `bvneg`, binary `bvadd`, `bvsub`, `bvmul`,
`bvand`, `bvor`, `bvxor`, `bvult`, equality, and Boolean connectives. It
does not implement the full logic: division, indexed operators, `let`, and
`define-fun` are unsupported. Search enumerates at most 16 referenced bits
and returns `unknown` on budget exhaustion. Use `get-model`, `get-value`, or
the C++ `last_bv_result()` accessor; `last_result()` is Boolean-only.

The session supports Boolean declarations and nonrecursive definitions,
simultaneous `let`, scoped assertions, assumption checks, and model/value
queries. It rejects unsupported commands or terms with an exception rather
than treating them as solved. For compatibility, the session allows an
omitted `set-logic` and enables model production by default.

Input and nesting limits are set via `smtlib2::Limits`. Sessions own their
state; mutating operations invalidate the last result. Script execution is
sequential: successful earlier commands remain effective if a later command
fails. Quoted and unquoted spellings of the same symbol share an identity.
`ScriptBuilder` builds Boolean scripts and `run_protocol` executes commands
from a stream. By default the driver throws on errors. Pass
`ProtocolErrorPolicy::report_and_stop` or `report_and_continue` after the
input limits to emit SMT-LIB `(error "...")` responses. Continuation is
limited to non-resource errors at the end of fully framed commands. No
external solver is launched.

## Compile the header as a CLI or REPL

From a directory containing a copied or installed `include/smtlib2` tree:

```sh
c++ -std=c++17 -DSMTLIB2_CLI  -x c++ include/smtlib2/smtlib2.hpp -o smtlib2-cli
c++ -std=c++17 -DSMTLIB2_REPL -x c++ include/smtlib2/smtlib2.hpp -o smtlib2-repl
printf '(assert false)(check-sat)\n' | ./smtlib2-cli     # unsat
./smtlib2-cli problem.smt2
./smtlib2-repl
```

The CLI reads one file, `-`, or stdin and streams responses to stdout. It
stops on the first SMT error (wire error on stdout, exit 1). Both tools
support `-h`/`--help`; invalid arguments produce a diagnostic and exit 2.
The REPL keeps one session, accepts multiline commands and multiple commands
per line, and prompts on stderr (`--no-prompt` disables this). Type `:help`
or `:quit` between commands; `(exit)` and clean EOF also end the session.
A fully framed non-resource SMT error permits another REPL command; framing
and resource errors stop it. The REPL uses ordinary stream input, without
terminal editing or history.

Select exactly one flag in exactly one translation unit for an executable.
Defining both is a compile error. Leave both undefined to use the library
with your own `main`, including in multiple translation units.

## Build and install

```sh
cmake -S headstone -B build/headstone -DCMAKE_BUILD_TYPE=Debug
cmake --build build/headstone
ctest --test-dir build/headstone --output-on-failure
cmake --install build/headstone --prefix /chosen/prefix
```

`cmake -S headstone/SMTLIB2 ...` also works. Consumers may use
`find_package(SMTLIB2 CONFIG REQUIRED)` and `Headstone::SMTLIB2`, or copy the
`include/smtlib2` directory. The example is `examples/boolean.cpp`.

To fuzz the lexer and parser with Clang, run from `headstone/SMTLIB2`:

```sh
clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
  -Iinclude tests/fuzz_parser.cpp -o fuzz_parser
./fuzz_parser -runs=50000
```

## Amalgam8 and Firestone

`amalgam8/Firestone.am8` selects Firestone's arbitrary-precision `BigInt`.
`cornerstone/Amalgam8` bundles it into the checked-in
`include/smtlib2/generated_firestone.hpp`, retaining its license notice.
`smtlib2::Integer` exposes this type. Exact integer and rational value utilities
include numeral and decimal parsing, Euclidean division, and rational
arithmetic. They are not integrated into a theory solver.
`smtlib2::BitVector` provides exact fixed-width value operations, including
SMT-LIB signed and unsigned division and remainder. Its operations are more
extensive than the `QF_BV` execution fragment described above.
Amalgam8 itself uses Firestone's LibDSL while generating the header. The
regeneration is a development-time operation; consumers need neither.

The opt-in executable modes additionally use a private C++17 profile of
LibDSL's native-schema `cli::OptParser` (both modes) and
`repl::Directives` (REPL directive tokenization, binding, and dispatch).
Amalgam8's catalog lists the full C++20 `LibDSL.hpp` as **not bundleable**.
`amalgam8/extract_frontend.py` instead extracts reviewed source ranges,
verifies source and MIT license hashes, and emits `generated_frontend.hpp`.
Its adaptations use C++17 lookup/prefix operations, plain schema descriptions
for help, and printable ASCII for user text in argument diagnostics.
It includes no third-party Unicode tables, terminal backend, or script runner.
The generated types live outside Firestone's public namespaces.

To verify the snapshot against a local Libstone tree:

```sh
cmake -S cornerstone/Amalgam8 -B build/amalgam8 -DAMALGAM8_BUILD_TOOLS=ON
cmake --build build/amalgam8 --target amalgam8
cmake -S headstone -B build/headstone \
  -DSMTLIB2_AMALGAM8_EXECUTABLE="$PWD/build/amalgam8/amalgam8"
cmake --build build/headstone --target smtlib2_regenerate_firestone
```

The target compares both generated headers against the checked-in snapshots; it does
not rewrite source files. See `AGENTS.md` and `@implstring` comments for the
implementation plan, contracts and outstanding limitations.
