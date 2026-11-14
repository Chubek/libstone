# C11 header-only contracts

## Declaration and implementation are different compilation profiles

A typical MuLIB header uses one guard for public declarations and a second
implementation switch. A C11 native compiler consumer should compile the
header with implementation macros undefined; precisely one implementation
translation unit defines all required `MULIB_IMPL_*` macros before including
the header. Exotract records this separation explicitly and rejects discovery
profiles that activate a configured implementation define or a `MULIB_IMPL_*`
name. The emitted TU is not safe to compile in multiple objects.

### MicroJS particularities

`mulib-microjs.h` publicly exposes opaque interpreter and object pointers;
`mulib_microjs_value_t` is an owned tagged result; its strings have byte
lengths and object/function handles are rooted. `mulib_microjs_value_free`
releases owned results. A native callback receives borrowed arguments and must
not reenter eval/call; collect is a no-op during execution. These are behavioral assertions of
the attached header, not consequences of its function pointer types.

Exotract does not automatically translate them into safe On1x ownership types.
Such translation must be grounded in a reviewed ExoML semantic-contract bundle
and corresponding compiler-side On1x ownership/runtime support.

## Manifest profiles

The versioned output contains the library name, normalized configuration,
source fingerprint, syntax validation result, extraction completeness, symbol
inventory and diagnostics. On1x projection emits C function names and CDecl
type IDs; the full CDecl graph is separately exposed to the Lua binder.

Compiler preprocessor output is useful for cache fingerprints and profile
inspection but does not replace compiler-derived ABI evidence. ExoML's schema
and trust rules remain authoritative; an external CDecl import is explicitly
marked and is never cached as native verification evidence.

## Attaching a semantic sidecar

`Bind({contract='api.exocontract.json', safe_exports='unsafe_explicit'})` uses
ExoML's existing schema v2 parser and validator. Its canonical serialization is
available as `Internals.contract`. A malformed sidecar, missing declaration,
parameter-count drift or rejected semantic invariant aborts the bind before
returning any artifact. No state is promoted to reviewed or usage-checked.
Even a sidecar claiming review cannot enable `reviewed_only`: exact source/ABI
matching, review provenance and the safe adapter lowering gate remain open.
