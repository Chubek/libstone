# Exotract architecture and invariants

## Scope

A C11 header-only library contract comprises header path and documentation;
preprocessing/profile identity; required dependencies; distinct declaration and
implementation compilation profiles; API declaration IR; semantic-contract
references; backend-specific generated artifacts; and provenance/verification.
Exotract's type authority is ExoML's owned `cdecl::TranslationUnit` schema v2.
Exotract must not invent a second canonical C11 type graph or erase unknown
ownership/effect semantics.

## API and lifecycle

`ExoLib` is defined in C++ by `Config` and `Engine`. `Preprocess` expands a
restricted `$GLOBAL{IDENT}` expression, validates input presence, then calls
ucpp/compiler through an argv-vector POSIX subprocess driver. `Parse` checks
C11 syntax with the configured compiler then optionally receives a complete
ExoML CDecl v2 graph. `Cache` persists only verified, complete declaration JSON;
`Bind` requires completed discovery and exposes no safe exports. Optional
`Bind({contract=path})` validates through ExoML's canonical `ContractBundle`
parser/validator and attaches normalized metadata to `Internals.contract`.
Linked SDK validation needs no executable; standalone validation uses a private
temporary output from `exoml-bind` and rejects every nonzero exit before reading
it. The validator's current structural checks do not authenticate a review or
verify every C ABI/ownership obligation; `reviewed_only` remains rejected.

Lua `ExoLib:Preprocess():Parse()` is implemented with userdata; Lua does not own
or modify the CDecl AST. The `Internals` table carries serialized authoritative
CDecl JSON and a separate On1x manifest of explicit unsafe imports. Custom Lua
binders receive it and return it (or a replacement table).

## Fundamental boundaries

1. **Declaration-only discovery:** `MULIB_IMPL_*` defines never enter the
   public extraction configuration. Only an independent generated C11 TU
   enables the implementation bodies.
2. **Exactly once:** emit the implementation TU once per program; duplicates
   must fail the linker test, never be silently de-duplicated in generated code.
3. **No ABI guessing:** type IDs, pointer widths, record layouts and callback
   semantics originate in compiler-derived ExoML CDecl and independent probes.
4. **No unsafe promotion:** CDecl and cached metadata remain discovered and
   unknown-effect. Only ExoML semantic-contract review can authorize safe exports.
5. **No shell interpreter:** executable paths and argv values are separate;
   macro expansion cannot turn a path into shell syntax.
6. **No fallback masking:** compiler failures, child timeouts and malformed
   CDecl JSON produce errors. Unavailable optional preprocessors may fall back.
7. **Versioned artifacts:** `moonstone.exotract/2` and
   `moonstone.exotract.on1x/2`; schema changes require explicit version bumps.
8. **Fresh cache evidence:** LMDB hits require matching freshly extracted CDecl
   bytes, preserving compiler/target/revision/input fingerprints and diagnostics.
9. **Probe evidence:** CParser success is reported independently; its unsupported
   C11 errors retain stdout and stderr. ucpp's C99 profile never replaces the
   authoritative C11 compiler preprocessor output.

## Next integration gates

- Gate A: real vendor execution, linked/CLI discovery, installed SDK consumption,
  cross-process LMDB cold/warm/invalidation and MicroJS native checks are now
  exercised. See STATUS for the remaining full-Clang-build and installed-probe
  deployment limitations; these results do not establish semantic safety.
- Gate B: enrich ExoML semantic contracts and bind approved functions to
  typed wrapper recipes; no raw-pointer default exports.
- Gate C: implement On1x compiler-side consumer of `on1x/2` plus actual typed
  On1x AST emission using the current On1x grammar and ownership rules.
- Gate D: diverse MuLIB fixtures including macro-dependent APIs, inline/static
  declarations, C11 attributes, callbacks and multiheader dependency graphs.
