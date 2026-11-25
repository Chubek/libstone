# Cephyr's Sea-of-Nodes MIR

Build the native model from the checked acsem document:

```sh
perl scripts/acsem/acsem.pl --emit json cornerstone/Cephyr/MIR.acsem -o MIR.json
python3 cornerstone/Cephyr/cmake/generate_mir_model.py MIR.json CephyrMIRModel.hpp
```

Include `CephyrMIRModel.hpp`. The header uses C++20 and defines
`cephyr::mir`. `lookup_opcode(wire_name)` and `opcode_info(Opcode)` return
`const OpcodeInfo*`, or null for unsupported operations. The descriptor fields
are `opcode`, `name`, `result` (`none|required|optional`), `fixed_inputs`,
`payload` (`none|word|symbol|call|return_value`), `effects`, and `successors`.
`is_ordered()`, `is_terminator()`, and `accepts_inputs(count)` are helpers.
Wire names are unchanged; the C++ members for alternative tokens and `const`
are `and_`, `or_`, `xor_`, and `const_`. Use `lookup_value_type` and
`value_type_name` for wire types.

### Graph storage

One `Graph` represents one function. The integration layer retains the
function name, result signature, locals, and module metadata for serialization.
`Graph::nodes`, `inputs`, `blocks`, and `successors` are flat vectors; references
are indices, so node insertion order does not constrain graph edges. Use
`Graph::add_node(Node)`, `Graph::add_inputs(span)`, and
`Graph::input_slice(InputRange)` (the latter returns an optional span), or
resize/populate the vectors directly to reserve forward references.

* `Node` fields: `kind`, `block`, `opcode`, `type`, `value`, `inputs`,
  `immediate`, `symbol`. The `value` stores an SSA value tag independent of
  its `NodeId`; lowering assigns compact wire IDs separately. Defaults are
  `no_value`, `ValueType::void_`, and
  `Opcode::invalid` when not applicable. `immediate` is a native `uint64_t`;
  `symbol` is an owning string. For calls put the indirect callee first (when
  applicable) in the data inputs; call arguments follow it in wire order.
* `Input { NodeId source, EdgeKind kind }` has `data`, `control`, or `effect`
  kind. Data positions are significant. An `InputRange { first, count }`
  is a slice of `Graph::inputs`, not of `Graph::nodes`.
* `Graph::start` names the single `NodeKind::start` node. ABI arguments are
  `NodeKind::parameter` with `block == no_block` and a control input from
  the start. Their `value` IDs give the argument order. All other nodes belong
  to a block.
* `Block { region, terminator, parameters }` has one `NodeKind::region` and
  one `NodeKind::terminator` (`br`, `cbr`, `ret`, or `trap`). `parameters`
  is a range of data references to its `NodeKind::parameter` nodes, in
  block-argument order. The first block is the function entry. Each region
  receives a control and an effect edge from each predecessor terminator
  (or start for entry).
* `Successor { terminator, target, arguments }` is a CFG edge. For a `cbr`
  append true before false, including when both target the same block.
  `arguments` is a range of data references, in destination parameter order.
  A terminator's own data inputs hold only its condition or return value.
* Each block parameter, operation, and terminator has a control input from
  its block region. Ordered operations form one effect chain from the
  region through all ordered operations in original order to the terminator.
  Do not put an effect edge on a pure operation.

`schedule(const Graph&)` in `cephyr/mir_schedule.hpp` returns an optional
vector of per-block operation IDs in deterministic data/control/effect
dependency order, or empty on invalid/missing dependencies. Serialize only a
successful schedule, then apply the Archon validation appropriate to the
backend. `OpcodeInfo` is graph/operand metadata, not an equivalence proof;
an unknown or unproved rewrite must not run. “Pure” alone does not authorize
reassociation, speculation, or deletion.

Cephyr uses this graph by default for C code generation. The
[optimization and lowering catalogs](../mir/README.md) are compiled into the
compiler. `--mir-opt` enables the integer rewrite catalog; `--no-mir-opt` (the
default) still runs the five lowering stages and emits Archon MachineIR v1.

The standalone `--emit cpp` acsem backend is a bounded command-line interpreter,
not a graph library. Its descriptor result protocol can be tested with
`--term 'DescribeOpcode(call_indirect())'`, which gives
`completed token:optional 1 token:call token:call 0`. Compilation does not run
this interpreter and does not inherit its 32-bit values or term/depth limits.

When Archon adds a wire operation, add its nullary `Opcode` constructor and
descriptor rule in `MIR.acsem`. The five descriptor fields are listed next to
the rules. Each currently supported payload is either absent, a 64-bit word,
a symbol, a symbol followed by variadic data inputs, or the optional return
value. Extend the projection and wire writer together for a new payload shape.
Classify operations with observable effects or non-total behavior as ordered;
new operations do not enable any rewrite on their own.

Run `python3 cornerstone/Cephyr/tests/test_mir_model.py` to check the
descriptor table against Archon's native wire operations and to compile and
test both the generated native header and the standalone acsem C++ interpreter.
