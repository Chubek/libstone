# RV64 native frame lowering

Frame offsets are relative to `sp` after allocating the frame. In order:

1. Outgoing stack argument words, starting at offset zero.
2. Spill and other stack-home words, starting at `slots_base`.
3. Temporary words for parallel edge copies, starting at `phi_base`.
4. Local objects in declaration order, each padded to its own alignment.
5. An aligned return-address word for functions that contain calls
   (`return_offset == -1` for leaf functions).
6. Padding to the LP64 stack alignment of 16 bytes.

Incoming stack arguments begin at the original `sp` and are addressed with
`frame_size + offset`. The layout reserves only the headroom needed to keep
the largest actual displacement within `int`. Local objects must be nonempty
and have power-of-two alignment no greater than 16 bytes. A function with no
stack usage need not adjust `sp`.

Before lowering, an independent verifier checks the actual region offsets
for alignment, bounds and overlap. It checks the phi-copy and outgoing areas
against the function's active edges and calls, requires a saved return address
exactly when the function calls another function, and validates the largest
incoming displacement. Stack-home indices are checked by the home verifier.

Register homes are `x29`–`x31`. Far accesses may use `x28` as scratch, but
preserve the other temporaries as well as a store's source and base. A far load
can form the address in its destination when that does not overwrite its base.
Constants use signed 12-bit digits materialized with `addi` and `slli`; far
addresses add the materialized displacement to the base with `add`.
