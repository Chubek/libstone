# Native MachineIR verification

The native backend verifies the input before allocation or emission. A failed
verification returns a diagnostic without publishing an artifact. It does not
use the legacy runtime Oracle or change the input.

## Borrowed input contract

For the duration of the call, the caller owns the module, its arrays and
NUL-terminated names. Nonempty arrays must point to readable, correctly
aligned storage for the specified element count. Neither another thread nor a
cancellation callback may change or release this storage during the call.
The verifier checks count limits, nullability and indices; it cannot determine
whether an arbitrary non-null C pointer actually refers to valid storage.
Empty arrays may have null pointers. A global with a positive word count and
no initializer pointer is zero-initialized; a zero word count declares an
external data symbol.

## Verified invariants

- Functions, imports and globals share a single namespace. A call names a
  function or import and matches its argument count, argument types and result
  type; void calls have no destination. `GLOBAL_ADDR` names a declared global.
- Each function has an entry block at index 0. It has no block parameters or
  incoming edges. Every other block is reachable from the entry. Block
  parameters are permitted on loops and joins.
- Every value ID has exactly one definition: an ABI argument, a block
  parameter, or an instruction result. A use in the defining block must
  follow the definition. A cross-block use requires the defining block to
  dominate the use block. Edge arguments are uses at the end of the source
  block and match the target parameters in number and type.
- `RETURN` matches the function result (using `ARCHON_MIR_NONE` for void).
  `BR` uses only its `yes` edge; `CBR` uses an `i64` condition and both edges;
  `TRAP` has no operands or edges. An unused terminator value must be
  `ARCHON_MIR_NONE`; inactive edges must have a null argument pointer and zero
  count, and their target indices are ignored.
- Instructions must follow the [native legality rules](Legalization.md).
  Verification does not establish runtime pointer validity, memory safety,
  arithmetic preconditions, or termination.

Module limits include 256 combined symbols, 65,536 instructions, and 65,536
global words. A function may have up to 256 blocks, 16,384 instructions, and
4,096 entries in each of its value and local tables. ABI signatures and call
argument lists may have at most 64 entries. Block parameter and edge argument
lists are bounded by the value table limit. A separate module work limit
bounds aggregate operand checking and dominance computation. Reachability and
dominance operate on bounded scratch storage without recursion. Cancellation
is checked during declaration, CFG and instruction verification.

After allocation, a separate location checker verifies unique stack homes,
reserved registers, block boundary constraints, and every register use by
tracking its physical contents across writes and calls. Emission validates
operand ranges against the target encoding tables. A bounded layout check
rejects duplicate or unresolved local labels and overlapping or out-of-range
transfer fixups before the backend publishes an artifact. The raw encoder
also checks that a fixup touches only its reserved placeholder bytes.
