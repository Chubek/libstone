# 7. Native target adapters

Include `<targets/Native.hpp>` and link `Flintstone::native`. This header is the
intentional boundary where existing Limestone and Sandstone types are exposed;
the generic IL and C ABI do not expose scheduler/allocator implementation types.

The native path is:

```text
verified, specialized Flintstone function
  -> application target selector / ABI lowering
  -> Schedrow region + RegTL function + SymAS form bindings
  -> verified scheduling
  -> CFG liveness and verified allocation
  -> checked form operands and Sandstone encoding
  -> relocatable bytes, symbols, relocations and backend reports
```

## An explicit target, not an architecture-name guess

```cpp
auto bank = std::make_shared<domsymas::Databank>();
auto status = bank->load_file("cpu.symas");
if (!status.ok()) throw std::runtime_error(status.message);

// IDs below are SymAS record IDs from the loaded bank: target 0 selects the
// first target record, and model 0 selects the scheduling model record whose
// issue width and unit capacities are imported.
constexpr std::uint32_t target_id = 0, model_id = 0;
flintstone::NativeTarget target;
target.bank = bank;
target.target_id = target_id;
target.machine = flintstone::sandstone_machine(*bank, model_id);
target.select = my_selector;

module.target = flintstone::sandstone_target(*bank, target_id, 64);
auto result = flintstone::compile_native(module, registry, target);
```

`sandstone_target` imports the bank's architecture identity, address width,
endianness and mandatory features. Its index-width argument is deliberate:
compiler index policy is not inferred from an ISA's pointer width. Requested
extra features must name records actually present in the bank. Architecture,
pointer layout and byte order must agree between the module and bank.

`sandstone_machine` imports an existing model's issue width and named unit
capacities. Missing models, zero capacities and duplicate resource names fail.
Alternatively supply an explicit `schedrow::MachineModel` from the target's
authoritative metadata. An explicit issue width is required; a missing timing
model is not silently treated as a plausible CPU.

The shared bank is read-only during compilation. Do not mutate the underlying
bank from another alias or thread while a backend uses it.

## Selector result

`NativeTarget::select` returns a `SelectedFunction`:

| Member | Existing representation |
| --- | --- |
| `scheduling` | `limestone::schedrow::Region`: selected instruction IDs, block layout, defs/uses, memory/control effects, dependencies, resources and grouping. |
| `registers` | `limestone::regtl::Function`: virtual registers, physical classes, constraints, CFG, transfers, calls/clobbers and liveness facts. |
| `encodings` | Map from scheduling instruction ID to selected SymAS form and encoder bindings. |

The selector determines which instructions implement the source computation.
It must also lower phis, honor the native ABI, materialize required transfers,
and supply correct effects. It may use Limestone's Unisel/Limeburg or another
application adapter, but Flintstone does not introduce an independent selector
or solver. ISA metadata and target patterns must justify selected operations.

The generic bridge cannot prove arbitrary callback-generated machine code
equivalent to the input. This is an explicit target-implementation boundary,
not permission to silently treat unsupported operations as no-ops.

### Stable identities and matching contracts

Every selected instruction must occur exactly once in the scheduling region,
allocation function and encoding map. The bridge compares defs, uses, early
definitions, ties, implicit physical state/clobbers and block membership between
the scheduling and allocation representations. Block successors and live-out
sets must agree. Scheduling requires an explicit entry-first block layout.

Selected effects cannot omit memory reads/writes, trapping, branch, return or
terminator flags present in the chosen SymAS form. The selector supplies fuller
memory and ABI contracts where the bank flags alone are insufficient. Delay-slot
forms are rejected by this generic path because they require a dedicated
materialization adapter.

## Timing import

```cpp
auto instruction = flintstone::sandstone_instruction(
    *bank, form_id, model_id, instruction_id);
instruction.block = block_id;
instruction.defs = {result_value};
instruction.uses = {left_value, right_value};
```

This helper imports concrete latency, throughput as `cycles/uops`, and the
bank's named read/write-unit reservations. It does not infer semantic effects,
operands, register constraints or control flow from the mnemonic. It rejects
missing timing, unknown/inapplicable units, zero cycle/uop values and variadic
latency. Model-specific scheduling must remain separate from ISA semantics.

The adapter's instruction issue demand is one selected instruction. Targets
with richer multi-uop issue, bundling or operand-latency models should supply
the corresponding Schedrow fields explicitly instead of relying on the helper's
simple timing projection.

## Scheduling and allocation

The bridge invokes `schedule_cfg`, then `verify_cfg`. Semantic dependencies,
memory effects, control flow and target resource constraints are retained.
RegTL instructions are reordered into the verified schedule before liveness
analysis. It then calls `regtl::analyze`, the selected allocator, and
`regtl::verify`.

Choose `Allocator::LinearScan`, `Greedy` or `Coloring`. Physical register IDs
refer to SymAS register records, **not** encoding numbers. Parent/subregister
overlaps in the bank are converted to RegTL alias pairs and merged with target
aliases. Fixed registers, ties, reserved registers and call clobbers remain
explicit allocation constraints.

If allocation reports spilled values, the generic adapter fails with
`native.spill`. It does not invent a stack frame, spill instruction, scratch
register or calling convention. Materialize spills/reloads through a target
adapter and submit a complete legal handoff. There is no post-allocation
reordering that could silently invalidate the allocation's live ranges.

## Form and operand bindings

A `FormEncoding` names one bank form and its encoder variables. Each
`FormBinding` has:

- `name`: variable consumed by the bank's encoding program;
- `position`: operand-descriptor position, or `domsymas::kNoId` for literal
  encoder metadata;
- `kind`: allocated virtual `Value`, explicit `PhysicalRegister`, or `Literal`;
- a value/register ID or `domsymas::encoding::Value` literal.

Virtual bindings must name a declared def/use of that instruction. Physical
bindings must occur in its implicit-state dataflow contract. The bridge looks
up the selected physical register and passes its `enc_num` to the encoder.
Extra unpositioned bindings may only be literal metadata.

Visible operand positions must be covered exactly once. The default checks
validate register class/width/read-write permissions, symbol presence and
signed/unsigned immediate range. A target proof is required only for nonempty
target-specific `constraint` strings; it can never override a failed basic
legality check. Unknown operand kinds are rejected outright. `prove_operand`
is a target proof, not a heuristic that the generic framework supplies
automatically. Missing proofs fail.

Required/forbidden form features are checked explicitly because calling an
encoder by ID alone is not evidence of feature legality. Sandstone then parses
and executes the form's own checked encoding program.

## Outputs and relocation ownership

`NativeCompilation` owns:

- an `Artifact` with `format == "stone-native-relocatable"`;
- concatenated bytes;
- function symbols with image-relative offset and size;
- relocations with image-relative offset, bank relocation name, symbol and addend;
- per-function selected region/provenance, form bindings, schedule, allocation
  problem and verified allocation reports.

Relocation fields must fit in the emitted instruction bytes, carry a non-empty
symbol, and not overlap another relocation in the same instruction. Offsets are
rebased when functions/instructions are concatenated. Required instruction
alignment is checked; a target must supply explicit padding rather than have
the bridge invent a NOP encoding. Block successors are compared as ordered
vectors because swapping branch targets would otherwise silently miscompile.

This artifact is **not** an ELF/COFF/Mach-O object or a complete linked image.
The consumer must preserve and resolve its symbols/relocations or hand them to
an object/linking adapter. Unresolved references must not be discarded before
execution. `register_native_backend(registry, name, target)` installs this path
behind the ordinary `emit` API for embedding.

## Shipped concrete targets

`<targets/SandstoneTargets.hpp>` (link `Flintstone::native`) ships three tiny
but real integer subsets with verified machine encodings:

| Backend | Registers | Integer subset |
| --- | --- | --- |
| `x86-64-tiny` | `eax/ecx/edx/ebx/esi/edi` | MOV-imm32, MOV, two-address ADD/SUB/IMUL, RET |
| `aarch64-tiny` | `w0`–`w7` | MOVZ, ADD/SUB/MUL, MOV-via-ADD, RET |
| `riscv64-tiny` | `a0`–`a7` | ADDI, ADD/SUB/MUL, RET-as-JALR |

Only classic low x86 registers are used: extended registers would need REX
prefixes and `esp`/`ebp` must never be clobbered by host-executed code. The
selector covers straight-line `i32` functions (constant/copy/add/sub/mul plus
return); branches, calls, memory, wider types and parameters fail explicitly
with `tiny.*` diagnostics. Immediates are range-checked per architecture
(x86 full `i32`, AArch64 `0..65535`, RISC-V `-2048..2047`).

```cpp
#include <targets/SandstoneTargets.hpp>

auto target = flintstone::make_tiny_target(flintstone::TinyArch::X86_64);
module.target = flintstone::sandstone_target(*target.bank, 0, 64);
auto result = flintstone::compile_native(module, registry, target);
```

`register_tiny_backends(registry)` installs all three under `emit()`; the CLI
accepts them with `--target/--backend`. Enumerate every shipped Sandstone
architecture (27 `.dsymspec` targets, from 16-bit MSP430/AVR to 64-bit
GPU/accelerator records) with `all_architectures()` / `find_architecture()`,
or list everything with `--list-targets`. All four RegTL allocators
(`LinearScan`, `Greedy`, `Coloring`, `PBQP`) are selectable per target.

## Executable evidence

`../tests/native.cpp` is a complete, runnable small-target example. It builds a
synthetic SymAS bank, selects a computation, schedules/allocates/encodes it, checks
the exact bytes and relocation rebasing, and checks negative contracts. The
shipped tiny targets are additionally checked byte-exactly against independent
bit-level expectations derived from the allocated registers, and the x86-64
subset executes on the host and returns 42.

On Linux AMD64 the synthetic test additionally supplies an explicit two-address
x86 subset: MOV-immediate, ADD and RET. RegTL ties the add destination/input and
fixes the return value to EAX. The test emits actual machine code, changes its
mapping from writable to executable, calls it and checks the result is 42. Its
synthetic scheduling policy is test data, not advertised processor timing
knowledge. These tests do not certify production backends or cross-architecture
execution; the tiny subsets, immediate ranges and single-block limits are
explicit, diagnosed boundaries.

Next: [Embedding](08-embedding.md).
