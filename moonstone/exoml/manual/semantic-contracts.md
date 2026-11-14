# Semantic FFI contracts

A semantic contract describes the *behavior* of a C function beyond its type
signature: ownership transfer, lifetime relationships, error conventions,
effects, callback policies, concurrency constraints and destructor obligations.
Contracts are separate from C declaration discovery; discovery is always
unsafe/unknown-effect and never promotes to reviewed semantics automatically.

## Contract model

`<exoml/Contract.hpp>` defines the typed contract model in namespace
`moonstone::exoml::contract`. The model is independent of the legacy
`OwnershipContract` in `SafetyEffects.hpp`; an explicit lossy projection
(`project_to_ownership`) bridges to existing code generation paths.

### Ownership and lifetime

Each parameter carries an `OwnershipKind`:

| Kind | Meaning |
|---|---|
| `scalar` | Trivial copy, no ownership semantics |
| `borrowed_immutable` | Const borrow; caller retains ownership |
| `borrowed_mutable` | Mutable borrow; caller retains ownership |
| `owned_unique` | Sole owner; must be released |
| `shared` | Reference-counted / shared ownership |
| `consumed` | Callee takes ownership; caller must not use |
| `out` | Callee writes; caller receives |
| `inout` | Caller provides; callee modifies in place |
| `opaque_unsafe` | Unclassified pointer; explicit escape hatch |
| `retained_callback` | Callee retains callback past the call |
| `transferred` | Conditional or unconditional move |

A `LifetimeSpec` ties a borrow or callback to a `LifetimeOrigin`: a specific
parameter, the receiver, the returned owner, global/static storage, a caller
buffer, a callback registration, or an explicit named region.

### Return contracts

`ReturnKind` distinguishes value returns, borrowed returns (with a required
lifetime source), owned returns (with a required `destroy_with` function),
nullable returns, and tagged-error returns. Owned returns must name a
destructor; deallocator families must be consistent.

### Error contracts

`ErrorKind` classifies how failures are reported: no error possible, sentinel
values, null-on-failure, boolean returns, integer codes, errno, output
parameters, or custom classifiers. Each error contract records whether output
parameters remain valid on failure and whether cleanup is required.

### Effects

`EffectKind` enumerates observable side effects: read, write, allocate, free,
escape, IO, thread, suspend, foreign_call. The `unknown` flag conservatively
treats the function as if all effects are possible.

### Callbacks and destructors

Callback contracts specify the exact C function-pointer signature, synchronous
vs. retained mode, user-data parameter, lifetime, and the required unregister
function for retained callbacks. Destructor contracts name the exact release
function, allocator family, idempotence, null safety, thread affinity and
whether drop may fail.

### Concurrency

Thread safety (single-thread, thread-safe, thread-affine, signal-safe),
registration-thread affinity, callback origin, blocking behavior and
suspension safety are explicit.

### Provenance and trust

Every contract carries provenance: source header, revision, source location,
author, review status (unreviewed, auto_inferred, reviewed, verified), evidence
method and toolchain fingerprint. The bundle-level `ContractState` tracks the
overall trust level: discovered, annotated, validated, reviewed, or
usage_checked.

## Sidecar format

Contracts are supplied as `*.exocontract.json` sidecar files. The format is
versioned (`schema_version: 2`); unknown keys are rejected. See
[contracts/schema.exocontract.json](../contracts/schema.exocontract.json) for
the complete reference.

```json
{
  "schema_version": 2,
  "library": "example",
  "headers": ["example.h"],
  "state": "validated",
  "functions": [{
    "c_name": "example_make",
    "return": {
      "kind": "owned",
      "destroy_with": "example_destroy",
      "deallocator_family": "malloc"
    },
    "parameters": [
      { "index": 0, "name": "size", "ownership": "scalar" }
    ],
    "errors": { "kind": "null_on_failure" },
    "effects": { "effects": ["allocate", "foreign_call"], "unknown": false },
    "provenance": {
      "header": "example.h",
      "revision": "abc123",
      "review_status": "reviewed",
      "author": "maintainer"
    }
  }]
}
```

## Validation (Gate B)

`validate_contract(bundle, unit)` checks a contract bundle against a CDecl
translation unit and returns diagnostics for every violation:

| Check | Diagnostic ID |
|---|---|
| Unsupported schema version | `EXOML-CONTRACT-VERSION` |
| Function not found in declarations | `EXOML-CONTRACT-DRIFT` |
| Parameter count mismatch | `EXOML-CONTRACT-DRIFT` |
| Same parameter both consumed and borrowed | `EXOML-CONTRACT-CONTRADICT` |
| Returned borrow without lifetime source | `EXOML-CONTRACT-LIFETIME` |
| Borrow references nonexistent parameter | `EXOML-CONTRACT-LIFETIME` |
| Owned return without destructor | `EXOML-CONTRACT-ALLOCATOR` |
| Destructor allocator mismatch | `EXOML-CONTRACT-ALLOCATOR` |
| Pointer/length parameter not found | `EXOML-CONTRACT-LENGTH` |
| Retained callback without unregister | `EXOML-CONTRACT-CALLBACK` |
| Duplicate function contract | `EXOML-CONTRACT-DUPLICATE` |
| Duplicate destructor | `EXOML-CONTRACT-DUPLICATE` |

Safe export (`is_safe_for_export`) requires every function to have review
status `reviewed` or `verified` and the bundle state to be `reviewed` or
`usage_checked`.

## CLI

```sh
exoml-bind validate --decl discovery.cdecl.json --contract example.exocontract.json
# Exit 0: valid; exit 3: validation failures; diagnostics on stderr.

exoml-bind validate --decl discovery.cdecl.json --contract example.exocontract.json --out normalized.json
# Writes deterministic normalized contract JSON.
```

## Deterministic serialization

`serialize_contract(bundle)` produces canonical JSON with alphabetical key
order, no optional whitespace, and stable enum names. Round-tripping through
`parse_contract_sidecar` preserves all semantic content.

## Projection to legacy model

`project_to_ownership(contract, parameter_count)` maps the rich contract into
the existing `OwnershipContract`:

- `consumed` / `transferred` parameters → `consumed_arguments`
- `borrowed_immutable` / `borrowed_mutable` → `borrowed_arguments`
- `ReturnKind::Borrow` → `returned_borrow`
- `ReturnKind::Owned` / `Nullable` → `may_escape`
- `EffectKind` → corresponding `Effect` enum values
- `effects.unknown` → `effects.unknown`

The projection is lossy; consumers that need the full contract should use the
typed `FunctionContract` directly.
