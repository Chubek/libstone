# Continuum: continuation and effect semantics

The current `OneShot<T>` is a thread-safe *single-use callback*, not a
function that can snapshot the native C++ stack. A successful CAS on `Ready`
transfers ownership of the callback execution to exactly one caller. Any
concurrent `resume` receives `AlreadyConsumed`. `cancel` only wins while the
continuation is ready. An exception is recorded and the continuation moves to
`Failed`; call `rethrow_if_failed()` to rethrow to a supervisory context.

`Prompt` checks a lifetime token before executing its callback. It does not
synchronize prompt destruction against a callback that is already running;
never use it as an enforceable cross-thread lexical resource boundary.

`Effects.hpp` routes synchronous named effects through nested handlers. It
provides exception-on-unhandled-effect behavior, but it does not itself capture
or resume a caller continuation. An effect handler may explicitly hold a
one-shot continuation supplied by a compiler/VM adapter.

`CPSMachine.hpp` supplies a stronger continuation reference machine. A program
consists of nodes containing simple integer operations and terminal transfers
(`Jump`, `BranchNonzero`, `Yield`, `Return`). A `Frame` is the value-state
snapshot. `run` stops at a yield and returns `Suspended`; the continuation ID
and registers persist in the frame. Resuming the frame advances execution.
Frames contain only pure integers and are copyable. Never generalize this
copying to futures, file descriptors, pointers, locks or unique borrows.

`weaver/AsyncAnalysis.hpp` implements backward fixed-point liveness to discover
which symbolic variables must be saved at suspension points, including
successor blocks. It does not generate C++ or SSA code from the specification.

`loom/Coroutine.hpp` uses the compiler's C++20 coroutine machinery; it is a
manually driven single-owner handle. `std::suspend_always` provides explicit
yield points. The handle must not be resumed concurrently or after its owner
is destroyed. The API is appropriate for reference examples and lowering
validation, not as a general asynchronous scheduler.
