# VMCollection

23 bounded, executable virtual-machine **profiles** described with the Lua
[VMWeave](../Weavers/VMWeave/VMWeave.lua) implementation. Each
description produces a standalone C99 runtime, a matching public header and a
STK-00 representation. There are no empty descriptions.

These are teaching/embedding models, **not drop-in implementations of the named
language platforms**. In particular, JVM/CLR/ART do not load class, PE or dex
files; WASM does not decode `.wasm`; LuaVM, BEAM and YARVM do not load their
platforms' bytecode files. VMWeave assigns its own opcodes. Instructions use
symbolic text and labels, not the original binary encodings. Comments in each
`.vmw` give the model, references and deviations.

## Generate and validate

From `lapislazuli/` (Lua 5.4 and a POSIX `ls` are required):

```sh
lua5.4 VMCollection/regenerate.lua
lua5.4 VMCollection/regenerate.lua JVM WASM
lua5.4 VMCollection/regenerate.lua --list
lua5.4 VMCollection/regenerate.lua --check
python3 VMCollection/test_collection.py
CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' python3 VMCollection/test_collection.py
```

The scripts also work when invoked by absolute path or from this directory.
`--check` never rewrites artifacts; it fails for stale or missing output.
Unknown options and VM names are errors. Generation validates all three forms
for deterministic emission, with canonical `VMCollection/...` source paths.

Tests require Python 3.10+ and a C99 compiler. `LUA`, `CC` and `CFLAGS` can be
overridden. Tests check every profile, strict C compilation, combined public
headers, regeneration/path independence, compiler rejection, error handling,
budget exhaustion, numeric edges, closures, calls, objects and backtracking.
Their temporary build files are isolated under `/tmp/opencode`.

Edit `.vmw` files, **not generated `.c`, `.h` or `.stk00` files**. Two related
families also share handwritten implementation models:

* [`object_machine.lua`](object_machine.lua): SOM/Smalltalk-80 objects and sends.
* [`logic_machine.lua`](logic_machine.lua): WAM/PROL terms, trail and alternatives.

Changes to those files require regenerating both profiles in the family.

## Profiles

| Directory / C prefix | Executable model | Important limits / exclusions |
| --- | --- | --- |
| ART | i32 register arithmetic, branches, static calls, result register | 16 registers/activation; no objects or wide values |
| BEAM | Tagged integers/lists, X/Y registers, calls, local mailbox | 16 X and 16 Y slots/activation; 512 list cells; 64 messages; no scheduler/selective receive |
| B-Machine / `BMachine` | Closures with copied free variables, arity-named application and recursion | 64 closures; 64 active bindings; 512 captured cells |
| C-Machine / `CMachine` | C-style stack frames, relative addressing and heap allocation in the VM stack arena | Fixed stack; no C source frontend |
| CAM | Tagged integers, pair-tree environments and unary closures | 512 pairs; 128 closures; no GC |
| CESK | Closures, mutable store, address-valued environments and continuations | 256 store cells; 64 closures; no store reclamation |
| CLR | i4 arithmetic, checked addition, locals/arguments, static calls | 16 locals; arity 0/1; no managed objects, metadata or exceptions |
| CMCM | Ground and multi-pair stacks, tuple projection, closures and calls | 256 multi-pair entries; 64 tuples/closures; 512 tuple fields |
| FAM | Tagged blocks, captured fields, application, tail application | 64 blocks of at most 8 fields; no partial application |
| FORTH | Data/return stacks, arithmetic, variables, loops, calls | 64 variables; no dictionary, parser, files or terminal I/O |
| FUL | Strict functional closure machine with persistent lexical bindings | Collection-defined dialect; 128 closures, 512 bindings; no GC |
| G-Machine / `GMachine` | Graph application/evaluation, update/indirections, basic values | 1024 nodes, 64-argument unwind spine; no general constructor/arity handling; no GC |
| JVM | i32 stack arithmetic, locals, branches, static invocation | Category-1 integers only; 16 locals; arity 0/1; no class loader/verifier |
| Krivine | Tagged call-by-name thunks, captured environments, explicit value channel | 128 closures/bindings; no call-by-need memoization |
| LuaVM | i64 registers, floor division, integer-key tables and closed functions | 16 registers; 64 tables with keys 0..15; no nil/mixed types/upvalues/metatables |
| P-Code / `PCode` | Local windows, indirect access and arity-named procedure calls | 16 cells/window; 512 locals including root; no static-link display |
| PROL | Variable/integer/list unification and explicit alternatives | Collection-defined dialect; same bounded engine as WAM |
| SECD | Closures, bindings, pairs and dump frames | 64 closures/bindings; pair allocation requires host allocator; no GC |
| SOM | Objects, fields, locals and one-argument selector sends | 64 objects/method entries; 16 fields/locals; no inheritance or blocks |
| Smalltalk-80 / `Smalltalk80` | Same bounded object model with Smalltalk-style words | No image loader, inheritance, blocks or non-local returns |
| WAM | References, integers/lists, unification, trail, choice points and calls | 16 X registers; 1024 terms; 32 choices; no general functors/Y environments/occurs check |
| WASM | i32 stack operations, locals/globals, linear memory and lowered branches/calls | One 64KiB page, 16 locals/globals; no binary decoder, validation, grow, floats, tables or imports |
| YARVM | Tagged integers/booleans/nil, truthiness, two-element arrays and closed methods | 64 arrays; 16 locals; no dynamic method lookup, blocks, strings or bignums |

FUL and PROL originally contained no dialect/version information. Their profiles
are explicitly collection-defined rather than attributed to an undocumented
historical ISA. SOM and Smalltalk share an object model, but retain separate
namespaces and mnemonic vocabularies.

## Embedding example

```c
#include "VMCollection/JVM/JVM.h"

int main(void) {
    JVM_vm vm;
    JVM_instruction instructions[32];
    JVM_tape tape = { instructions, 0, 32 };
    JVM_init(&vm);
    if (JVM_compile("ICONST 41 INVOKESTATIC1 &inc PRINT HALT "
                    "inc: ILOAD 0 ICONST 1 IADD IRETURN", &tape)) return 1;
    if (JVM_run(&vm, &tape, 100)) return 2;
    return vm.out == 42 ? 0 : 3;
}
```

Compile from `lapislazuli/`, for example:

```sh
cc -std=c99 -I. example.c VMCollection/JVM/JVM.c -o example
```

The same interface pattern is generated for each C prefix. `NAME_lookup` and
`NAME_opcode_WORD` expose the actual metadata/opcodes; consult the generated
header for handler signatures. Execute through `NAME_run` or `NAME_step` so
stack-effect validation runs, rather than calling unchecked handlers directly.

## Execution contracts

* A tape operand is a cell, or an instruction-index label (`&name` in text).
  A label must identify an instruction, not the position just past the tape.
  The compiler accepts whitespace and `#` line comments. It is an assembler
  for the profile, not a language compiler or bytecode verifier.
* All mutable state belongs to the VM instance. Initialize with `NAME_init`.
  Stacks, tables, stores and heaps are bounded. Hosts must not corrupt counters,
  object handles or instruction metadata. These are not security sandboxes.
* Every instruction's declared stack effect refers to **VMWeave's value stack**.
  Register machines and private graph/multi-pair stacks declare `( -- )` and
  check their own storage. Call instructions consume arguments immediately;
  return instructions deliver results later. Callees must balance temporaries.
  C-Machine's RETURN/RETURN1..3 also consume its four organizational cells and
  the stated argument count; discard local cells before returning.
* `PRINT` (or `WRINT`) records the most recent integer in `vm.out`; it does not
  print to stdout. Program output is inspected by the embedding host.
* Closure snapshots preserve lexical bindings across later calls; CESK captures
  **store addresses**, so mutation remains shared. Activation snapshots restore
  caller bindings rather than just their former height. FUL uses persistent
  linked binding records instead of copying environments.
* JVM/ART i32 arithmetic wraps, including MIN/-1 division. CLR/WASM signed
  division traps for that overflow. LuaVM wraps i64 arithmetic and uses floor
  division; it correctly treats integer zero as truthy. Ruby-style branches
  distinguish false/nil from integer zero.
* Tagged integer profiles reserve three low bits, leaving a signed payload of
  cell width minus three bits; overflow wraps the payload rather than allocating
  a bignum. Older `intptr_t` profiles use unsigned intermediates for wrapping
  arithmetic and conventional two's-complement integer conversion.
* WAM/PROL failure restores bindings, registers, heap extent and call return
  addresses from the latest choice. TRY/CHOICE installs an alternative,
  RETRY changes it, and TRUST/COMMIT removes it. CUT discards **all** choices,
  not just those of the current predicate. Acyclic terms are the supported
  domain; worklists and dereference chains are bounded.
* SECD defaults to a custom allocator. Before `CONS`, install callbacks with
  `SECD_memory_set`; release pair objects with `SECD_object_clear` before
  resetting/destroying the VM. Other profiles use fixed per-instance arrays.

Common status values are `-2` (underflow), `-3` (capacity), `-4` (bad operand,
address, type or assembly), `-6` (declared stack-effect mismatch), `-7` (step
budget exhausted), `-8` (allocation), `-10` (arithmetic trap) and `-11` (terminal
logical failure). Runtime semantic errors are sticky until reinitialization;
budget exhaustion is resumable at the next instruction. Errors need not roll
back arbitrary instruction-side mutations; logic backtracking is explicit.

## Extending a profile

1. Add state and authoritative semantics to its `.vmw` (or shared family model).
2. Declare operand kinds, actual value-stack effect and control-flow metadata.
3. Check capacities, live handle ranges, types and arithmetic traps before access.
4. Add executable success and failure cases to `test_collection.py`; every VM
   directory must have test coverage.
5. Regenerate, run `--check`, then run the normal and sanitizer test suites.

Native file loaders, complete language libraries, verifiers, collectors and
platform conformance suites remain separate work; generation success does not
assert platform compatibility.
