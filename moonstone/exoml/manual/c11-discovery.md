# C11 discovery: initial P0.1 implementation

## Design and invariants

Input is one header plus explicit include directories, object-like defines,
undefines, optional target triple and Clang resource directory. libclang parses
in C11 mode, following the AST-discovery approach inspected in MuLIB's
`mudsl/scripts/generate_mulib.py`. No shell, compiler plugins, implementation
switches, generated programs, or header-provided scripts are executed.

Output is an owned, versioned `cdecl::TranslationUnit`: declaration/type graph,
source spelling and canonical identity, qualifiers, typedef edges, fields,
arrays, callbacks, enum values, compiler-measured layouts, diagnostics and input
fingerprints. The public model contains no libclang types. It is separate from
the compatible `BindingModule` projection, whose string fields cannot represent
this information losslessly. Discovery establishes **no semantic contracts**:
all declarations remain discovered, unsafe, and unknown-effect.

Compiler errors preserve the partial graph but make `complete=false`. Unknown
types/declarations retain their spelling and an actionable unsupported reason.
Resource budgets fail closed. Exceptions never cross libclang visitor callbacks.
Compiler layout failures are absent values, never fabricated zero sizes.

The extraction library/tool is opt-in. libclang and OpenSSL (SHA-256 input/type
fingerprints) are developer dependencies only; the existing runtime, generated
static bindings, and installed `Moonstone::ExoML` remain independent of them.

Tests cover typedef chains, qualification, recursive/opaque records, callbacks,
arrays, union/bitfield/packed/aligned shapes, target widths, conditional APIs,
invalid source/options, budgets, determinism, and real pinned MuLIB inventories.

## Deliberate limits

The initial pinned-MuLIB acceptance gate A is now verified; see the
[closeout design/report](discovery-gate-a.md). Parse-free TU caching, nested
macro replacement trees, arbitrary flags and untested compiler/platform profiles
are not claimed. Fingerprints identify parsed input. The
implemented [normalization cache](discovery-cache.md) also validates a freshly
serialized compiler AST before reuse: discovery always reparses (including include resolution).
No binding generator consumes discovered declarations yet. Layout evidence is
for libclang's selected target only, not GCC/MSVC equivalence or executed probes.
Packed/union/bitfield/callback types are discovered, not authorized for adapters.
Headers may read other files through includes; run discovery in an isolated
process/filesystem for hostile input. Budgets bound exported data, not Clang's
internal parser memory/time. Stable inputs/files are required during parsing.
