# Generating a standalone C99 emulator

Archon can turn validated encodings and SemIR into a standalone C99 translation
unit. `gen` still emits only the codec by default; add `--emulator` for
instruction execution and `--header` for a separately includable API.

From `cornerstone/Archon`:

```sh
python3 -m archon compile examples/toy8.arch -o toy8.archdb
python3 -m archon asm-program toy8.archdb examples/toy8.s -o toy8.bin
python3 -m archon gen toy8.archdb --emulator --prefix toy8 \
    -o toy8_emu.c --header toy8_emu.h
cc -std=c99 -Wall -Wextra -Werror -pedantic -I. \
    examples/toy8_runner.c -o toy8-runner
./toy8-runner toy8.bin
```

The [runner](examples/toy8_runner.c) prints `r1 = 254`, `r2 = 6`, and
`r3 = 4` among the final registers. Generated code needs neither Python nor an
ArchDB at runtime.

## APIs

`--prefix toy8` makes functions and types start with `toy8_` and status
constants start with `TOY8_`. The default prefix is `archon`.

The codec API is unchanged:

| Function | Purpose |
| --- | --- |
| `toy8_encode(...)` | Encode one named instruction |
| `toy8_decode(...)` | Decode one exact instruction |

The original register-list emulator API is also unchanged:

| Function | Purpose |
| --- | --- |
| `toy8_emu_register_count()` | Number of architectural register slots |
| `toy8_emu_register_width()` | Register width, or zero when there is no register file |
| `toy8_emu_execute(name, args, count, regs, reg_count)` | Execute by name |
| `toy8_emu_step(bytes, size, regs, reg_count)` | Decode and execute exactly one instruction |
| `toy8_emu_run(bytes, size, regs, reg_count, &executed)` | Execute a linear encoded stream |

Old callers therefore require no source changes for register-only
architectures. On an architecture with extended state, this API can still
execute an instruction that does not read or write extended state. It returns
`TOY8_EMU_STATE_REQUIRED` instead of silently discarding scalar, stack, or
memory effects.

The additive complete-state API is:

| Function | Purpose |
| --- | --- |
| `toy8_emu_execute_state(name, args, count, &state)` | Execute by name against complete state |
| `toy8_emu_step_state(bytes, size, &state)` | Decode and execute one instruction |
| `toy8_emu_run_state(bytes, size, &state, &executed)` | Execute a linear stream |

Operands remain positional `uint64_t` values in declaration order. A signed
immediate uses its sign-extended bit pattern; for example, `UINT64_MAX`
represents `-1`.

## Constructing state

The header contains an architecture-specific `struct <prefix>_emu_state`, a
common `struct <prefix>_emu_stack`, and enum constants describing all fixed
bounds. For a hypothetical `vm` target, Archon names are preserved after a
category prefix:

```c
uint64_t registers[VM_EMU_REGISTER_COUNT] = {0};
uint64_t values[VM_EMU_STACK_Values_CAPACITY] = {0};
uint8_t ram[VM_EMU_MEMORY_RAM_SIZE] = {0};
struct vm_emu_state state = {0};

state.registers = registers;
state.scalar_PC = 0;
state.scalar_Flags = 0;
state.stack_Values.values = values;
state.stack_Values.size = 0;
state.memory_RAM = ram;
```

For a registerless target, `VM_EMU_REGISTER_COUNT` and
`vm_emu_register_width()` are zero and `state.registers` may remain null.
Each scalar is a `uint64_t` bit pattern checked against
`VM_EMU_SCALAR_<Name>_WIDTH`.

A stack buffer has exactly `VM_EMU_STACK_<Name>_CAPACITY` entries. Active
entries occupy `values[0]` through `values[size - 1]`; the latter is the top.
Only active entries are value-validated, but the whole capacity must be valid
storage because an instruction may push.

A memory pointer addresses exactly `VM_EMU_MEMORY_<Name>_SIZE` bytes.
Addresses are byte offsets. Multi-byte loads and stores use the architecture's
declared endianness.

The generated C cannot discover the size of a pointer's allocation. Callers
must provide every required buffer at its generated bound. State buffers must
not overlap one another or the encoded input stream.

## Transactions and errors

Every instruction runs on a private deep snapshot. Registers, scalars, stack
contents and depths, and all memory bytes are committed together only when the
instruction returns `*_EMU_OK`. This includes failures after earlier semantic
writes, conflicting parallel writes or stores, stack underflow/overflow, and
out-of-bounds memory access.

`*_emu_run_state` commits each successful instruction separately. On failure,
earlier instructions remain committed and `bytes_executed` reports their byte
prefix; the failing instruction is rolled back. The legacy `*_emu_run` has the
same prefix behavior for registers.

Relevant statuses include:

- `*_EMU_INVALID_ARGUMENT`
- `*_EMU_UNKNOWN_INSTRUCTION`
- `*_EMU_OPERAND_RANGE`
- `*_EMU_NO_SEMANTICS`
- `*_EMU_CONFLICTING_WRITES`
- `*_EMU_TRUNCATED_INSTRUCTION`
- `*_EMU_ALLOCATION_FAILURE`
- `*_EMU_STATE_REQUIRED`
- `*_EMU_STACK_UNDERFLOW` / `*_EMU_STACK_OVERFLOW`
- `*_EMU_MEMORY_OUT_OF_BOUNDS`

The complete-state functions allocate scratch storage proportional to the
declared register file, all stack capacities, and all memories. `run_state`
allocates it once and reuses it for the stream. Every allocation is checked and
released; allocation failure leaves caller state unchanged. Each instruction
still copies the complete modeled state. Parallel memory stores additionally
use bounded automatic staging proportional to the bytes stored by that
parallel block.

## Exact limits

- Generated codecs support byte-aligned fixed-width encodings up to 64 bits.
- Generated execution supports SemIR values, registers, scalar fields, stack
  elements, addresses used by semantics, and memory accesses up to 64 bits.
  Wider state or intermediate values are rejected at generation time.
- Stack capacities and memory sizes are fixed by the architecture. The current
  frontend bounds them at 65,536 entries and 16 MiB per memory respectively.
- `step_state` and `run_state` consume the supplied instruction bytes linearly.
  A scalar named `PC` has no special behavior: there is no implicit increment,
  branch-driven fetch, or instruction fetch from modeled memory.
- Flags and other control fields are ordinary typed scalars. There are no
  interrupts, exceptions, device/volatile memory, atomics, concurrency, or
  operating-system services.
- A SemIR instruction omitted from the architecture still decodes but returns
  `*_EMU_NO_SEMANTICS`.
- Generated state is caller-owned. There is no generated allocator,
  deallocator, loader, or default-state constructor.

Within these limits the output is standalone ISO C99 and is tested with GCC
and Clang using `-std=c99 -Wall -Wextra -Werror -pedantic`.
