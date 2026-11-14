# ExoML implementation status

This expansion completes a usable **static binding path**, not the full ExoML
workstream or Moonstone's production-safety acceptance plan.

| Implemented | Verification |
| --- | --- |
| Aurocks grammar → ASDL AST → source-preserving commands | Generator dependencies; `exoml_parser` syntax/location/budget/concurrency tests |
| Typed native auto-binding and checked conversions | `exoml_autobind`, including invalid input before invocation |
| Capabilities, thread affinity, exception diagnostics | `exoml_autobind`, original interpreter test |
| Atomic batch/module binding, filtering/renaming, aliases/constants | `exoml_autobind`, `exoml_generated` |
| Structured declaration validation and reflection | `exoml_static_ffi`, manifest JSON in `exoml_codegen` |
| Compiler-checked generated wrappers | `exoml_generated`: generated code linked to real C11 fixture |
| Compiler-measured ABI and layout expectations | `exoml_codegen`: probe JSON, deliberate signature/layout compile failures |
| Declarative workspace, Types helpers, transactional edits | `exoml_static_ffi` |
| DSL-to-wrapper path and CLI | `exoml_script`: CLI output compiled, linked and invoked |
| CLI/line REPL error handling | `exoml_cli`, including malformed input and resource limits |
| Installed CMake package | `exoml_package`: independent consumer configures, links and executes |
| Independent public headers | `moonstone_exoml_header_checks`, warnings as errors |

## Open work

- C11 preprocessing/header extraction with ucpp/CParser, declaration discovery
  from headers, package/version discovery.
- General pointer/handle ownership, aggregates by value, callbacks, variadic
  marshalling and calling-convention extensions. Explicit host adapters remain
  necessary; runtime ABI layouts are never guessed.
- DynamicFFI, extension loading/process isolation, CPython/Ruby emitters and
  XBind Lua integration. ExolangTk's inspected call engine reports `FFI_ENOSYS`;
  this implementation uses compiler-generated static calls.
- Pipelines, nested calls, arithmetic expressions, guards, imports, compile-time
  evaluation and the full proposed language. Unsupported syntax is rejected;
  `.g`/`.asdl` now describe the implemented syntax.
- Stable C embedding ABI/plugin table, replxx editing/history, native-call
  cancellation, production fuzzing/audit and cross-platform CI.

## Local verification

- Linux GCC: all 36 standalone Moonstone CTests pass with tools/Python enabled.
- Linux Clang 22: all nine ExoML checks pass with AddressSanitizer and UBSan.
- Generated wrappers compile and invoke a real C library. Incorrect native
  signatures/layout expectations fail compilation, and probe JSON matches the
  independently compiled fixture's layout.
- Public ExoML headers compile independently on both compilers with warnings
  treated as errors. No complete LibStone root build or Windows validation is
  claimed by this increment.
- Clang static-analyzer checks pass on the interpreter, parser bridge,
  declaration/code generation implementation and standard-library workspace.
