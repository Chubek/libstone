# RV64 LP64 calling convention

The native backend targets little-endian RV64IM with the LP64 integer calling
convention. Machine IR supports `I64`, `PTR`, and `FUNC_PTR` values. All have
64-bit representations and occupy naturally aligned 8-byte words in stack homes,
argument slots, and global word arrays. Global words are laid out consecutively
in declaration order within each array, least-significant byte first. Defined
arrays with initializers are placed in `.data`; zero-initialized arrays are in
`.bss`. An array declaration with zero words refers to external data. A `VOID`
result has no storage. Explicit frame locals are byte ranges with the size
and alignment given in their declarations.

## Calls and returns

Functions and imports use fixed signatures with at most 64 scalar parameters.
Argument indices are zero-based:

| Argument index | Location at callee entry |
| --- | --- |
| `0`–`7` | `a0`–`a7` (`x10`–`x17`) |
| `8`–`63` | `sp + 8 * (index - 8)` |

The stack grows downward and `sp` is 16-byte aligned. The caller reserves
outgoing space at offset zero in its frame, sized for the largest call in the
function. It places stack arguments there before filling the argument registers.
There is no shadow area for register arguments and no padding between stack
arguments. After the callee allocates its frame, it reads stack parameters from
`sp + frame_size + 8 * (index - 8)`. The callee copies all incoming parameters to
its own stack homes before executing the first block.

A scalar return value is in `a0` (`x10`); a void call does not read a result.
GAS emits a direct call as `call symbol`, a relocatable PC-relative
`auipc`/`jalr` pair. `ARCHON_MIR_FUNC_ADDR` produces a `FUNC_PTR` to a named
function or import. The static linker resolves both PC-relative references;
in [raw output](BinaryEmission.md#raw-instruction-bytes) both refer to the
current function and are resolved during emission. Unlike a data `PTR`, a
`FUNC_PTR` is not an address for memory operations or pointer arithmetic.

`ARCHON_MIR_CALL_INDIRECT` uses its symbol only to identify a declared function
or import signature. The verifier rejects known signature mismatches; for
runtime targets, the caller must supply a `FUNC_PTR` whose target matches the
signature. After marshaling arguments, the backend reads the target into `t2`
and emits `jalr ra, 0(t2)`. Both call forms preserve the caller's own return
address in its frame. Variadic calls, floating-point arguments, aggregates,
and multi-register scalars are not supported.

`t0`–`t2` (`x5`–`x7`) are reserved scratch registers, and `t3` (`x28`) is
reserved for frame-address and branch expansion. `t4`–`t6` (`x29`–`x31`) can
hold block-local values, but no value live across a call is kept in them. No
callee-saved general-purpose registers are allocated.
