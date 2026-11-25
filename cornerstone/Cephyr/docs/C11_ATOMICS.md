# C11 atomics

The bundled Linux AMD64 and AArch64 backends support `_Atomic T` and
`_Atomic(T)` for integer (including `_Bool`) and object-pointer types. Their
atomic objects have the same size, natural alignment, and representation as
the corresponding non-atomic types. Fixed arrays of atomic scalars,
initializers, pointers to atomic objects, and const and volatile atomic
qualifiers are supported. Atomic lvalue conversion and simple assignment
perform sequentially consistent loads and stores. Integer `++`, `--`, `+=`,
`-=`, `&=`, `|=`, and `^=` use a single read-modify-write operation; they do
not support `_Bool`.

The bundled `<stdatomic.h>` provides the standard integer atomic typedefs,
`memory_order`, initialization and lock-free queries, loads, stores,
exchange, strong and weak compare-exchange, integer fetch operations,
`atomic_flag`, and thread and signal fences. Weak compare-exchange uses
the strong operation. Explicit orders must be integer constant expressions;
the frontend rejects invalid orders and invalid compare-exchange
success/failure combinations. The runtime intentionally strengthens valid
orders to sequential consistency (signal fences remain compiler-only).
The header supplies function-like macros, not addressable functions.
Pointer fetch operations, other compound updates, atomic floating-point,
function-pointer and aggregate types, atomic record members, and by-value
atomic parameters and results are not supported.

Lowering uses impure calls on the existing Archon wire, so even discarded
reads and fences survive optimization, and no new wire operations are
required. The runtime uses width-specific lock-free machine operations and
does not require `libatomic` or a thread library. Links that omit the bundled
runtime must provide the compiler's atomic helper symbols from the matching
runtime archive. Target packages built before this implementation must
rebuild the archive.

`__STDC_NO_ATOMICS__` remains defined for the bundled RV64IM target, which
lacks the A extension, and for other unreviewed backend routes. The subset
above is not a claim of complete C11 conformance.
