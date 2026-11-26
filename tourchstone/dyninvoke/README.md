# Tourchstone dyninvoke

`tourchstone/dyninvoke` is a libffi/dyncall-style foreign-call library over
Yellowstone qBFD and Sandstone ISA data.

- qBFD's object-aware qABI classifier identifies a supported calling
  convention from object metadata, not from the file extension or CPU
  family alone.
- Sandstone `.isa` files (`sandstone/isa/x86.isa`, `aarch64.isa`,
  `riscv64.isa`, `riscv32.isa`) are loaded at runtime to cross-check the
  architecture identity, register names, classes, widths and numbers when
  `dinv_isa_regcounts` is requested. The `DINV_ISA_DIR` environment variable
  overrides the default source-tree or configured install path.
- Layout queries (register/stack locations and hidden struct-return pointers)
  are available cross-host for the implemented subset of each ABI.
- Native calls and closures are enabled only on Linux SysV AMD64. Other
  layouts are inspection-only; `dinv_abi_can_execute` and
  `dinv_abi_can_close_over` expose the distinction.

| ABI | Fixed-signature layout coverage |
| --- | --- |
| SysV AMD64 | Integer/SSE eightbytes, whole-aggregate spill, hidden returns |
| Microsoft x64 | Positional register slots, stack beyond shadow space, direct small aggregates and hidden returns |
| AAPCS64 | Nested homogeneous FP aggregates, eight-byte spill strides, hidden returns via `x8` |
| Darwin ARM64 | Nested homogeneous FP aggregates, naturally aligned compact fixed-argument stack, hidden returns via `x8` |
| RISC-V LP64D and ILP32D | XLEN-specific pointer/layout widths, hard-float flattening and integer fallback, register/stack split, hidden returns |

## C API sketch

```c
const dinv_type_t* args[] = {&dinv_type_i32, &dinv_type_f64};
dinv_cif_t* cif = NULL;
dinv_cif_create(DINV_ABI_SYSV_AMD64, &dinv_type_i32, 2, args, &cif);
int32_t a = 2; double b = 2.5; int32_t r = 0;
void* argv[] = {&a, &b};
dinv_call(cif, fn, &r, argv);
dinv_cif_destroy(cif);
```

Structs are described with NULL-terminated element lists:

```c
static const dinv_type_t* pairElems[] = {&dinv_type_i32, &dinv_type_i32, NULL};
static const dinv_type_t pairT = {DINV_STRUCT, pairElems};
```

Call interfaces deep-copy their type graph; temporary descriptors may be released
after creation. Closures retain a signature snapshot and may outlive the source
cif; the application must keep their callback and context alive until all
invocations have stopped and the closure has been destroyed. A raw `dinv_call`
function address is borrowed; keep the supplying module loaded throughout the
call. Cyclic graphs, empty records, void fields/arguments, invalid kinds,
nesting beyond 64 levels and values larger than 4096 bytes are rejected. Null
argument-value storage is rejected before marshalling.
`dinv_type_named` returns borrowed immutable builtin descriptors without an
out-pointer. `dinv_type_layout` queries target-specific natural record size and
alignment; the legacy size/alignment helpers assume the host pointer width.
Argument and return slot queries expose payload sizes and field offsets, not
carrier padding. Stack counts omit final call-frame padding and Win64's 32-byte
shadow space.

Win64/AAPCS64/Darwin/RISC-V indirect aggregate arguments are explicitly
unsupported. Varargs, packed or over-aligned records, vectors, long double,
nontrivial C++ objects and RISC-V soft/single-float ABIs are not modeled. The
caller must supply the exact fixed C signature; object identification cannot
verify a function prototype. Native functions/callbacks must not throw across
the C/assembly boundary.

## Tool

`dinvoke --inspect` and `--layout` use only offline inspection; neither invokes
the native loader, even when `--arg` options are given. `--layout` accepts an
explicit ABI override as a hypothetical layout; calls reject an override that
disagrees with the inspected image. Invocation checks the dynamic symbol is a
defined function, then keeps the native dynload handle alive until the call
returns. Only call trusted modules with known signatures.

```sh
dinvoke --inspect --layout ./libfn.so my_add
dinvoke --ret i32 --arg i32:2 --arg i32:5 ./libfn.so dinvk_add2
dinvoke --abi sysv_amd64 --ret f64 --arg f64:1.5 --arg f64:2.25 ./libfn.so dinvk_addd
```

## Yellowstone extension

`yellowstone/qbfd/qABI.hpp` (included by `qBFD.hpp`) provides shared ABI IDs,
names, ISA metadata and host detection. For cross-target layout comparisons,
compile `tests/abi_oracle.c` with Clang `--target=<target> -O1 -S`, passing
`-march=rv32gc -mabi=ilp32d` or `-march=rv64gc -mabi=lp64d` for RISC-V.

## XBind spec

`DynINVOKE.xbind` describes the callback-free ABI/type/layout query subset for
all XBind backends: D, Go, Java, Lua, NodeJS, OCaml, Python, Ruby, and Rust.
Direct calls, closure creation, `void **` argument arrays, and executable
function pointers are intentionally deferred until XBind IR supports callbacks
and buffer/out-parameter lowering.
