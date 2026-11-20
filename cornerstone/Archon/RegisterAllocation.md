# RV64 native register allocation

`an_allocate` operates on verified Machine IR. Each SSA value gets one
function-wide home: a register in `t4`–`t6` (`x29`–`x31`), or a unique
8-byte stack slot. `t0`–`t2` are operation temporaries; `t3` is reserved for
frame-address and control-transfer expansion. Stack homes are numbered in
value-ID order. Frame lowering handles actual loads, stores, and register
moves at definitions and uses.

The allocator collects all definitions before scanning uses, so layout order
need not follow dominance order. ABI parameters, block parameters, operands
of edge parallel copies, and any value used outside its defining block are
forced to stack. A value used after an intervening call is also forced to
stack: the bank is caller-saved. The indirect callee pointer and all call
arguments, including arguments passed on the stack, are read before the call
clobbers registers. An operand at its final use may stay in a register, and
a call result may start a new register lifetime after the clobber. Conditional
predicates and return values are read at the end of their block.

A linear scan tries the lowest available register for each remaining
instruction definition in block order. Reads at an instruction precede the
definition's write, so an input at its final use may share the output's
home. For a given register, the scan admits a new definition only once the
old interval's last read is at or before the new write, or after evicting
the old interval. The register bank starts empty for each block.

If all three registers are live, the scan chooses the active interval with
the latest last use, breaking ties by lowest register number. The new
interval takes its register only if the new interval ends strictly sooner;
otherwise the new value gets a stack home. Eviction is an offline decision:
the evicted value has one stack home from its definition through its last
use, with no mid-interval spill or allocation state passed across edges.
This prioritizes shorter intervals but does not try to minimize dynamic
load/store counts.

An independent checker simulates ordered reads, writes and call clobbers
against the final homes, resetting register contents at each block and
requiring stack homes for edge arguments. It also checks stack slot bounds
and uniqueness. There is no live-range splitting, coalescing, stack-slot
reuse, critical-path weighting, or global liveness analysis. The passes take
linear time in values, instructions, and operands and use one bounded heap
table of live ranges. The allocator polls for cancellation at block boundaries
and periodically within large blocks.
