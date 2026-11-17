# Continuations and synchronous generators

`callcc f` evaluates `f` and requires a closure, then applies it to a singleton
tuple containing the captured continuation. The capture excludes the callcc
action itself and includes pending combinator states, lexical environments,
syntax nodes, received transients, intermediates and function boundaries.

`resume k with v` evaluates k then v and abortively replaces the current control
stack. A fresh result `completed(v, {})` is delivered to the saved caller. The
continuation is one-shot: a second resume semantically fails. An invalid operand
also fails. Capturing or restoring beyond heap/frame budgets is a resource error.
Snapshots copy control state but retain shared references to immutable
environments and the **current shared store**; store writes are never rolled back.
No stack copying, setjmp/longjmp or process-pointer serialization is used.

The examples `Context()` and `Replay()` in `continuations.acsem` both print
`completed 42`; Replay stores the continuation, resumes after callcc returns,
and checks that a second resume fails.

`generator A` captures an initially unexecuted action frame. `next g` restores
its saved segment above the caller's next delimiter. `yield_value v` saves the
segment excluding the yield action and returns `1 v` to that caller. On the next
resume, yield completes with an empty tuple and empty bindings. Normal completion
returns `0` followed by its completion data; failure/escape propagates and closes
the generator. Subsequent next returns just `0`.

`close g` discards suspended state and is idempotent on a closed generator.
Next/close of a running generator, or yield outside next, fails. Nested generators
use the nearest next delimiter. Return inside a generator completes its body.
`Sequence()` in `generators.acsem` prints `completed 1 20 1 22 0 99 0`.

**Limitations:** no cleanup handlers, async generators, generator delegation,
multi-shot/composable continuations, shift/reset or algebraic effect handlers.
Specifications mixing callcc/resume with generator primitives are refused during
synthesis on every target; cross-delimiter lifecycle semantics are not implemented.
