# 6. Execution and bytecode

## Execute a module

```cpp
flintstone::Registry registry;
auto module = flintstone::parse(source);
auto answer = flintstone::execute(module, registry, "affine",
    {std::int64_t(10)}, 1000000);
auto number = std::get<std::int64_t>(answer);
```

Execution specializes and verifies a private module, lowers high-level
operations, compiles verified Firestone SSA, links functions, installs runtime
intrinsics and runs Firestone's register VM. It does not mutate the source
module. Target `vm` is required; a native target is not silently retargeted.

Supported executable values are:

- `i1` as C++ `bool`;
- `i2` through `i64` as `std::int64_t` signed bit-pattern representations;
- `f64` as `double`;
- `ptr<generic>` as Firestone `Address` handles into an `Environment`;
- void as `std::monostate`.

Arguments must match the signature and narrow integer widths. A pointer is a
VM heap-cell address, **not** a host/native pointer. Aggregate values, opaque
types, wider integers and non-binary64 floats require explicit backend lowering.

## Integer semantics

The builtin arithmetic contract is modular integer arithmetic:

- add/subtract/multiply/negate wrap at their declared width;
- bitwise operations operate on that width's bit pattern;
- ordered comparisons are signed;
- division/remainder are signed, truncating toward zero;
- minimum-signed divided by `-1` wraps to minimum-signed; its remainder is zero;
- division/remainder by zero fail;
- left shift wraps the width-limited bit pattern;
- right shift is arithmetic/sign-extending;
- shift counts outside `[0, width)` fail.

Literal and argument values use the signed range, except Boolean literals are
only zero/one. For example, the eight-bit all-ones pattern is `-1:i8`, not
`255:i8`.

### Why explicit intrinsics are needed

Firestone's native scalar arithmetic implementation uses checked signed
arithmetic. Reusing it unchanged would make overflow trap before a Blackstone
rewrite but wrap after a modular rewrite. Flintstone therefore transforms
integer add/subtract/multiply/divide/negate/left-shift operations into declared
native runtime intrinsics named, for example, `__flintstone_mod_add_i32`.
Their implementation uses defined unsigned arithmetic and explicit width
normalization, never overflowing signed C++ operations. Remainder (`mod`) needs
no wrapper: its result always fits and `MIN%-1` is already defined as `0`.
Arithmetic right shift (`shr`) is already sign-extending with no overflow case,
matching modular signed semantics, so it also retains the Firestone
implementation. These are explicit dependencies in the emitted bytecode, not
hidden behavior attached to the host's ordinary arithmetic opcodes.

The `__flintstone_` function namespace is reserved. Applications cannot declare
functions there, and only the trap intrinsic is callable from IL: it takes no
arguments and yields the enclosing function's result type, which is exactly
what the `cf.trap` lowering produces. The remaining intrinsics are internal to
the backend's own arithmetic and width-guard lowering; their names and
signatures are validated when they are bound. `cf.trap` lowers to a call
to `__flintstone_trap` (which always throws `vm.trap`) followed by a return of
a dummy value, so trapping control flow composes with the ordinary call/return
machinery at every result type, including `void`.

Narrow integer parameters and values entering from host calls or heap loads
(`call`, `load`, and symbol copies) also receive `__flintstone_check_iN`
guards. There is no globals table in the IL, so there are no globals to guard.
FSVM function signatures record only scalar runtime kinds, so these explicit
guards retain IL width checks even when the emitted file is executed directly
through Firestone. `i1` values use the VM boolean representation and are not
covered by these integer guards; `i1` host arguments must already be booleans.

## Compile and consume FSVM bytecode

```cpp
#include <libvm/BytecodeBinary.hpp>
#include <libvm/RegisterVM.hpp>

auto artifact = flintstone::emit(module, registry, "vm");
auto bytecode = firestone::vm::decode(artifact.bytes);
firestone::vm::Environment environment;
flintstone::bind_vm_runtime(environment, bytecode);
auto result = firestone::vm::RegisterVM{}.run(
    bytecode, bytecode.find("affine"), {std::int64_t(10)}, environment);
```

When consuming an emitted FSVM file directly, **call `bind_vm_runtime`** on its
execution environment. A plain Firestone VM without those declared bindings
will correctly diagnose unbound natives. `flintstone::execute` performs this
step automatically. Intrinsic registration is not a portable machine-code or
dynamic-loader dependency.

`emit(..., "vm-stack")` compiles the same semantics to Firestone's stack model;
consume it with `StackVM`. Alternatively call `compile_vm` with the explicit
`firestone::vm::VMModel` to obtain an in-memory module.

FSVM encoding/decoding and structural/bytecode verification are Firestone APIs.
The format records constants, function signatures, native declarations and code.
Loading an FSVM file does not execute arbitrary serialized C++ callbacks.
The host supplies declared external functions separately.

## Calls, recursion and phis

Flintstone compiles each function against declared signatures, then links
internal calls by stable function identity. Forward calls, self recursion and
mutually recursive functions are supported. Calls and returns are checked
against complete IL type signatures before the VM's runtime-kind checks.

Loop phis are lowered by Firestone's compiler using split edges and saved
parallel sources. This preserves simultaneous phi-copy behavior, including
critical edges and loops. Flintstone does not serialize phi copies as an
incorrect sequence of destructive register moves.

A nonvoid VM definition must contain a return from which Firestone can infer
its declared result signature. A divergent void loop is supported and stops at
the execution budget. General resumable execution is a Firestone API rather
than the convenience `execute` API.

## Host callbacks and memory

```cpp
firestone::vm::Environment environment;
environment.bind("host", {{firestone::vm::ValueKind::Integer},
                          firestone::vm::ValueKind::Integer},
    [](const std::vector<firestone::vm::Value>& args) -> firestone::vm::Value {
        return std::get<std::int64_t>(args.at(0)) + 2;
    });
auto answer = flintstone::execute(module, registry, "external", {},
                                  1000000, &environment);
```

The IL must contain the matching external declaration and call signature.
Unbound symbols, mismatched runtime kinds and out-of-range narrow integer values
fail. A host callback remains responsible for its own semantics and effects. It
is not sandboxed or automatically rolled back.

`Environment::allocate`, `load`, `store` and `release` manage checked VM heap
cells. `mem.load/store` use these cells. This is not byte-addressed native
memory, and it does not model hardware atomics, volatile access or a native
memory ABI. A dedicated dialect/native adapter is needed for those contracts.

## Budgets and failure

The convenience API defaults to one million executed instructions. A budget
stop becomes `vm.budget`, not a successful partial answer. Firestone also limits
call and operand depths. Native callbacks count as VM instructions but their
internal runtime is application-controlled; the budget is not an operating-system
timeout or a sandbox.

Compilation/conversion is transactional. Runtime effects on a supplied
environment or external callback are **not** undone if later execution fails.
Use separate environments or application transactions when effect rollback is
required.

Next: [Native targets](07-native-targets.md).
