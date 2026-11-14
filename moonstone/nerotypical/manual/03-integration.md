# Integrating Sequent IR with LibStone

The upstream Nerotypical AST/checkers are currently source-to-constraint
components, not typed Sequent IR producers. The adapter separates these two
concerns instead of assuming the solver's satisfying model is an inferred
polymorphic program type.

## Type system integration

- `sequent/Bridge.hpp` accepts an inferred-type callback for each demo AST
  expression, an optional function signature callback, and an optional string
  literal contents callback.
- `sequent/TypeAdapters.hpp` converts ground Satie structural results into
  Sequent `Type` nodes, and singleton set-typing witnesses into ground types.
  General set-typing unions are not silently collapsed into ground types.
- A correct pipeline invokes the original structural/set checker first,
  verifies its status is `WellTyped`, constructs a typed tree or types oracle,
  then lowers with `AstBridge` and verifies the Sequent module.
- The callback is required because the original demo language does not expose
  solved type information for *every* expression as a typed AST annotation.
  This is a genuine frontend integration task, not a missing function call.

## Include and build

With all upstream files restored in the same directory, the umbrella header
`nerotypical/Nerotypical.hpp` exposes the original modules and Sequent IR.
`sequent/SequentIR.hpp` is intentionally Satie-free for standalone compilation.

```cmake
# Under moonstone/CMakeLists.txt (or your preferred workspace):
add_subdirectory(nerotypical)
target_link_libraries(mycompiler PRIVATE Nerotypical::Sequent)
```

When using the optional `TypeAdapters` or `Bridge`, also expose the original
Nerotypical and vendored Satie include directories.

## Compatibility

No original type checker functions, option names, or source AST node types
are renamed by this overlay. Existing `tests/set_typing.cpp` should continue
to compile in the fully restored repository. Only the small umbrella header
is intentionally changed to add `sequent/SequentIR.hpp`.
