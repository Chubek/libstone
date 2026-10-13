# XBind initial validation regimen

This regimen keeps the scaffold useful while the real C parser, XBind IR, and
target emitters are being implemented.

## Stage 0: scaffold integrity

Goal: prove that the tree has enough structure for contributors to start work.

Checks:

- `xbind --check-layout <root>` succeeds.
- `xbind --check-spec hello <root>/examples/hello` succeeds through QaMRpp.
- `python tests/regimen.py --root <root> --cli <built-xbind>` succeeds.
- Every supported target has a compiler directory, DomBind directory, and default
  typedef map.
- `XFeats.schema.json` parses as JSON.

## Stage 1: Lua spec contract

Goal: make `.xbind` files deterministic before executing user code in the full
driver.

Checks to add next:

- Load `lxbind/lxbind.lua` with a stock Lua interpreter.
- Load `examples/hello/hello.xbind` through QaMRpp and verify it returns a
  manifest table.
- Reject missing library names, empty target names, malformed params, duplicate
  function names, and type overrides without C/native sides.

## Stage 2: C surface ingestion

Goal: establish the XBind IR independently of target languages.

Checks to add next:

- Parse `examples/hello/hello.h` and preserve source spans.
- Match spec functions to C declarations by name.
- Diagnose missing declarations, ambiguous declarations, unsupported C types, and
  conflicting ownership annotations.

## Stage 3: type-map convergence

Goal: make target types explicit and overrideable.

Checks to add next:

- Parse each `typedefs/*-Default.sexpr` file.
- Apply spec-local overrides after defaults.
- Report the final C-to-native map per target.
- Fail on unmapped ABI-visible types unless the spec marks them opaque.

## Stage 4: DomBind lowering

Goal: make one target work end-to-end before expanding.

Recommended first target: Lua, because `domluabind` has a compact native surface.

Checks to add next:

- Lower the hello example into a target-neutral XBind IR dump.
- Generate Lua glue through `compilers/Lua.c` and `dombind/domluabind`.
- Compile the generated module and run a Lua smoke script.

## Stage 5: all-target smoke matrix

Goal: keep the SWIG-like promise honest across the configured targets.

Checks to add next:

- Build generated bindings for every target whose SDK is available.
- Run one smoke test per target for functions, records, strings, buffers,
  callbacks, error paths, and ownership transfer.
- Skip only when the native SDK is absent; compiler/runtime failures are hard
  failures.

## Definition of done for the first real slice

- `examples/hello/hello.xbind` generates and runs in at least one target.
- The target compiler emits through DomBind rather than hand-written glue.
- Default typedefs and user overrides are both exercised.
- Generated traces show the selected spec, headers, targets, type maps, and
  emitted files.
