# GateML

GateML specifies reusable language frontends: lexical rules, labeled grammars,
AST node kinds, semantic lowering hooks, and profile metadata. A definition
compiles to a backend-independent descriptor; source programs parse to owned
trees and can lower to Jiterati IR.

## Build

Requirements: CMake 3.20+, C/C++17 compilers, a D compiler (`ldc2`, `dmd`, or
`gdc`), and Python 3.11+ for tests. Dependencies are taken from this checkout;
the build performs no downloads.

```sh
cmake -S gatestone -B build/gatestone -DBUILD_TESTING=ON
cmake --build build/gatestone --parallel
ctest --test-dir build/gatestone --output-on-failure
build/gatestone/gateml/gateml-cli check gatestone/gatebindle/C
```

`gatestone/gateml` also builds standalone. The root Libstone build exposes
`-DLIBSTONE_BUILD_GATESTONE=ON`; it defaults to OFF.

## Definition

```gml
frontend Arithmetic;
lexer {
    skip WS = "[ \t\r\n]+";
    token NUMBER = "[0-9]+";
    token PLUS = "\\+";
}
parser {
    start expression;
    rule expression = expression PLUS atom => Add | atom => Pass;
    rule atom = NUMBER => Integer;
}
ast { node Add; node Pass; node Integer; }
adapter {
    lower Add = add;
    lower Pass = identity;
    lower Integer = integer;
}
profile { option language = "Arithmetic"; }
```

Definitions use one `frontend Name;` header followed by repeatable sections.
Every production declares its AST kind. Precedence and associativity are
expressed structurally through nonterminal layers and recursion.

## Commands

```sh
gateml-cli check path/to/frontend.gml
gateml-cli compile gatestone/gatebindle/C > C.frontend.json
gateml-cli grammar gatestone/gatebindle/C > C.ng
gateml-cli parse gatestone/gatebindle/C expression.c
gateml-cli lower gatestone/gatebindle/C expression.c
```

`compile` emits a deterministic, versioned frontend descriptor. `grammar`
emits NovoParse grammar over token codes. `parse` emits source-tree JSON;
`lower` emits a zero-argument `i64` Jiterati function. Commands accept a
single definition file or a bundle directory, and support `--help`.
Exit codes are 0 for success, 1 for input/compilation failure, and 2 for usage.

## Implementation

```text
GateML text ──libchomsky3──> token tape ──NovoParse/libglr──> Frontend
source text ──libchomsky3──> token tape ──NovoParse/libglr──> owned Tree
Tree + declared adapter hooks ──Jiterati builders──> semantic IR
Frontend ──versioned JSON──> future facility backends
```

`GateML.g` is compiled by the vendored NovoParse D frontend during the build.
Its generated C++ parser and the source-language parsers use NovoParse's native
runtime linked against **`third_party/libglr`**, rather than NovoParse's nested
copy. libchomsky3 owns regex compilation and matching for both GateML and the
defined languages. NovoParse sees only a one-byte token tape; it does not
retokenize original source with its internal regex scanner. The tape-to-source
map restores terminal lexemes, byte spans, and diagnostic locations.

GateML links Jiterati's header-only core directly. Lowering produces real
`jiterati::Module`, `Function`, `Block`, and `Value` objects, which can be handed
to Jiterati's backend APIs. The returned module has stable heap ownership.
Facility backends can supply named `LoweringHooks` callbacks and emit their own
Jiterati operations. Unknown hook names remain in frontend descriptors and
produce a located error if lowering is attempted without an implementation.

The C reference bundle supports decimal integer expressions, unary signs,
parentheses, `+`, `-`, `*`, `/`, `%`, and C comments. The remaining bundles
extend the same expression-seed pattern with language-specific lexing:

| Bundle | Frontend | Distinctions |
|---|---|---|
| `gatebindle/C` | `C` | C comments; additive/multiplicative/unary layers |
| `gatebindle/C++` | `Cpp` | `//`, `/* */`; bitwise, shifts, `~`, assignment, calls, variables |
| `gatebindle/Java` | `Java` | Adds unsigned `>>>` (`lshr`); `>>` lowers to `ashr` |
| `gatebindle/Rust` | `Rust` | `!` for bitwise negation; `//`, `/* */` |
| `gatebindle/Python` | `Python` | `#` comments; right-associative `**` (`pow` facility); `//` |
| `gatebindle/Ruby` | `Ruby` | `#` comments; right-associative `**` (`pow` facility) |
| `gatebindle/Delphi` | `Delphi` | `//`, `{ }`, `(* *)`; case-insensitive `div`, `mod`, `shl`, `shr`, `and`, `or`, `xor`, `not`; `:=` |
| `gatebindle/Typescript` | `Typescript` | C-style comments, `$` identifiers, arithmetic/bitwise/assignment/calls, `>>>` |
| `gatebindle/Scheme` | `Scheme` | Prefix integer arithmetic, signed literals, `;` comments, named calls |

The infix bundles beyond C bind `assign`, `variable`, `call`, and `callempty`
facilities for later backends; Python and Ruby additionally bind `pow`.
Scheme instead binds `variable`, `namedcall`, and `namedcallempty`.
These parse without host hooks and lower once an embedding application
supplies them. All seeds share the signed `i64` semantic model with
documented per-language approximations (see each bundle's `README.md`).
Full language coverage — declarations, types, statements, and standard
libraries — remains future work.

## D reference library

The `.d` modules now implement an independent reference library, built as
`libgateml-d.a` alongside the C++ bootstrap. Import `gateml.Interface` with
`gatestone` on the D import path. Its API includes `compile`, `load`, `parse`,
`grammar`, `lower` (portable bytecode), `evaluate`, visitors and tree rewrites.
It does **not** replace the C++ CLI or its Jiterati integration.

```d
import gateml.Interface;
void main() {
    auto frontend = load("gatestone/gatebindle/C");
    auto tree = parse(frontend, "1 + 2 * 3");
    assert(evaluate(frontend, tree) == 7);
    auto program = decode(encode(lower(frontend, tree)));
    assert((new VM).run(program) == 7);
}
```

For LDC, link a consumer with `ldc2 -Igatestone example.d
build/gatestone/gateml/libgateml-d.a -L-lrt` (adjust the build path as needed).

The reference lexer uses Phobos regex and requires UTF-8 input. Its regex
dialect can differ from libchomsky3 for definitions beyond the supplied seeds.
Definition and source text are validated as UTF-8, and definition diagnostics
track line/byte-column incrementally while pointing at the offending character.
The chart parser supports left recursion, epsilon rules and ambiguity rejection,
with explicit token, work, tree-size and tree-depth limits in `TargetSpec`;
it is correctness-first, not
intended for large source files. Tree offsets remain byte-based. VM arithmetic
wraps at 64 bits; invalid signed division and shifts trap. Host facilities are
resolved at execution time; lowering hooks can inspect the source tree and emit
custom bytecode. A running VM cannot be re-entered or reset by its callbacks;
facility arguments are borrowed and must be copied if retained. Execution uses
a snapshot of the caller's instructions. The C++ bootstrap still produces
Jiterati IR, not bytecode.

Clone, visitor, rewrite and lowering entry points reject null/cyclic trees and
bound expanded tree size/depth, including DAGs with shared subtrees. Rewrites
validate their results before returning them.

`Visitor.gml` fragments use ordinary profile options: `visitor_order`,
`visitor_terminals`, and `visit_NODE` handler names. The D visitor consumes this
metadata; C++ preserves it in descriptors without changing parsing/lowering.

The `serde` modules use `std.json.JSONValue` as their common data model:

* JSON is deterministic; `readDescriptor` reloads validated frontend descriptors.
* CBOR and MessagePack implement their JSON-compatible native binary types.
* BSON supports object-root documents, rejecting unsigned numbers beyond i64.
* TOML uses a `gateml_json` string envelope to preserve JSON nulls/mixed arrays.
* YAML supports the YAML 1.2 JSON-compatible flow subset only.
* FlatBuffers uses `table Envelope { json:string (required); }`, root type
  `Envelope`, file identifier `GMLF` (see `serde/Envelope.fbs`).
* S-expressions use tagged objects/arrays/numbers and JSON-escaped strings.
  Import `gateml.serde.SExpression`; `S-Expression.d` is a compatibility source.

JSON parsing uses strict RFC 8259 syntax. Encoders and decoders bound size/nesting
and reject unsupported binary types rather than
silently coerce them. TOML/YAML are not general-purpose parsers.
FlatBuffers accepts standard alignment padding and vtables on either side of
the root table; unlike framed bytecode, it is not a self-delimiting stream.

The `ipc` modules provide **Linux** owning handles for Unix sockets, pipes,
FIFOs, PID-file locks, POSIX shared memory, named semaphores and message queues.
Call `close()` deterministically (typically with `scope(exit)`); destructors are
fallback cleanup. Named resources are created exclusively; only creators unlink
them, including after `fork()` (inherited child handles do not unlink the
parent's resources). Filesystem IPC paths are anchored to their creation
directory and checked for replacement before unlinking. Use a trusted/private
directory: the identity check cannot eliminate concurrent filesystem races.
Shared-memory access needs caller synchronization. Pipes/FIFOs require a
caller-managed SIGPIPE policy. PID lock files deliberately persist after close
to avoid unlink/recreate races; PID reads reject non-regular, symlinked and
oversized files. Queue operations are nonblocking by default.

Linux CTest includes D frontend/VM/serde/IPC tests, real child-process IPC, and
independent Python readers checking emitted binary formats and TOML validity.

See [the language specification](specs/GateML.md),
[the EBNF](specs/GateML.ebnf), and
[the dependency integration notes](docs/dependencies.md).
