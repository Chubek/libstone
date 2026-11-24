# Closures, async functions, and tasks

These are **action-language** features of generated Python 3 tools. They work
in semantic/lowering/analysis actions and helper functions; they do not add
closures or threads to the input language or emitted StackIR programs.
C99, C11, C++17, and C++20 tool targets reject these execution features during
both `--check` and generation. Existing synchronous named functions and
function references remain supported on native targets.

## First-class lexical closures

```text
.function counter(start: Int) -> Fn()->Int
let value = start;
return fn() -> Int {
    value = value + 1;
    return value;
};
.end
```

In an action:

```text
let next = counter(10);
let first = next();     // 11
return next();         // 12
```

The expression syntax is `fn(name: Type, ...) -> ReturnType { statements }`.
Parameters and returns are explicitly typed; `Fn(Int)->Int`, nested function
types, vectors of functions, and functions returning functions are supported.
Calls check arity and argument/return types at generation time. Runtime
invocation also checks arity (including calls made by an embedding host).
Every non-`Void` function/closure must return or throw on all statically
recognized paths.

Closures capture **bindings**, not just current values. Assigning a captured
local updates its shared binding cell; sibling closures observe that update,
and the cell survives its defining call returning. Separate factory calls have
separate cells. Parameters and `let` declarations inside a closure shadow
outer names; a local's initializer can still read the outer binding before
the new declaration. Ordinary calls and `:` calls use lexical capture. A
direct `.` call on a member is the exception described below.
Only names referenced freely by the closure are retained. `$grammar` captures
retain their bound values too. `@state` and `#tables` use the owning context.

Like existing actions, control-flow statement bodies execute in the current
function/action environment; this feature does not introduce new block scopes.
Local recursive declarations and forward local references are not provided.
Named function recursion retains the existing depth bound.

Closures belong to one `Context` execution. An embedding host may retain and
invoke a returned closure using `ctx.invoke(closure, arguments)` until the next
reset/interpret/compile/analyze call on that context. Invoking it through another
context or after a reset raises `LanguageError`, rather than using stale state.

## Hybrid tables and method calls

`%Table` is a mutable, first-class value, distinct from a vector (`[...]`)
and a declared `.symtbl`. A `[[...]]` literal mixes keyed and unkeyed
entries:

```text
.state @data: %Table = [[ "a": 1, 12, 44, "b": fn() -> Int { return 0; } ]];
```

Use `[ [1] ]` for a vector containing a single vector, rather than the
table literal `[[1]]`.

Unkeyed entries are assigned integer keys starting at **1**; keyed entries do
not advance that counter. `@data.a` and `@data["a"]` read the same field.
`@data[1]` is `12`, while `@data[2]` is `44`. Assignment updates the table in
place; a missing key reads as `nil`, and assigning `nil` removes an entry.
Tables compare by identity. The `in` operator checks for a key, `for` iterates
own keys in insertion order, and `len` counts own entries.

Calling the member directly selects its scope:

```text
.function make(seed: Int) -> %Table
    return [[ "read": fn() -> Int { return seed; } ]];
.end

// In a semantic action:
let values = make(10);
let seed = 32;
let caller = values.read();  // 32: resolves seed in the caller's frame
return caller + values:read(); // 42: the second call sees make's seed
```

A direct dot call does not use lexical captures or expose metavariables. A
colon call uses the closure's definition-site bindings (shared cells), and
provides these metavariables to the called function:

| Metavariable | Value |
| --- | --- |
| `*This` | The original receiver, including when a method is inherited; a library receiver exposes its exported functions, state, and symbol tables. |
| `*Args`, `*Argv` | The explicit arguments as a zero-based vector, without the receiver. |
| `*Type` | The called function's signature. |
| `*Constructor` | The receiver table's prototype, or `nil` without one. |

Set a prototype using the reserved `"*Constructor"` key. Reads of absent
keys then fall back to the prototype; writes always affect the receiver.
For instance, if `prototype` defines `score`, the call in
`let child = [[ "*Constructor": prototype ]]; return child:score();`
looks up `score` on `prototype` but binds `*This` to `child`. Accessing a
member without immediately calling it simply returns the stored value;
calling that value separately is an ordinary call with no metavariables.
Use a colon to call an async method: a task takes a snapshot at call time
and cannot retain the live caller frame required by a dot call.
A synchronous `fn` written directly in a table literal can also refer to
caller-only locals. Such references raise an error in a lexical call.

First-class tables and call metavariables require the Python tool target.
Dot projection on nominal product and sum values also works on native targets.

## Async functions and task values

```text
.async function compute(value: Int) -> Int
discard sleep(0.05);
return value * 2;
.end
```

An async function's declared return type describes its eventual result:
`compute(21)` has type `Task<Int>`, not `Int`. Anonymous async functions use
`async fn(value: Int) -> Int { ... }`.

```text
let a: Task<Int> = compute(20);
let b = compute(21);
discard spawn(a);
discard spawn(b);
return await a + join(b);   // 82; both tasks run concurrently
```

* Calling an async function creates a **lazy, single-shot task**. Its body has
  not run yet, but its input/context snapshot has already been taken.
* `spawn(task)` starts a task on its own Python thread and returns that same
  task. Starting an already started/completed task is harmless.
* `await task` and `join(task)` start it if necessary, wait, and return its
  result or raise its exception. Await is a unary expression with call/index
  precedence, tighter than arithmetic; use parentheses where needed.
* Await/join are also allowed in synchronous functions and rule actions.
  They are deliberately blocking bridges, not Python `asyncio` coroutine
  objects. Multiple spawned tasks genuinely overlap blocking work; Python's
  GIL means this is not a promise of CPU-bound parallel speedup.
* `sleep(seconds)` is a blocking primitive, useful for timers/concurrent
  actions. It accepts finite numeric seconds in `[0, 1]` and returns `Void`.

All started tasks, including descendants, are joined before a rule-tree walk
or initialization finishes, including its error path. An unobserved exception
from a started task fails the execution. An unused, never-started lazy task
does no work and raises no body exception. Multiple joins are allowed.
There is no cancellation, detached task API, shared-memory task API, or
task-handle transfer between tasks.

## Isolation and safety contract

At async **call** time the runtime deep-copies the parent's state, tables,
arguments, closure binding cells, and grammar capture values into a child
context. Aliasing within that snapshot is preserved; aliases back to mutable
parent values are not. Parent writes after task creation are not visible to
the task. Task writes never automatically merge back into the parent.
Communicate through returned values:

```text
let values = [1];
let update = async fn() -> Vec<Int> {
    values[0] = 99;
    return values;
};
let task = update();
let result = await task; // [99]
return values[0];        // still 1
```

Each join copies the completed result into the receiving context; modifying a
joined vector does not modify the cached result of subsequent joins. Returned
closures are rehomed to that context with isolated captured cells. Task
handles cannot be copied into another task or returned across its boundary;
doing so raises a `TASK` diagnostic. Non-copyable host/foreign values also
produce a diagnostic rather than being silently shared.

Independent `Context` instances can execute concurrently. A single context
must not be driven/reset concurrently by multiple embedding threads.
Snapshot isolation covers **DSL-managed values**, not external side effects:
`print` output order across tasks is unspecified. Foreign/native hooks, if
enabled separately, must be safe for threaded invocation; this is not a
sandbox or a transaction system for host resources.

Bounds per execution (shared across the complete task tree):

* At most **64 created tasks**, including unused lazy tasks.
* The existing **1,000,000 action instruction** budget is aggregate across
  parent and task contexts.
* At most **128 synchronous function frames** per context.
* At most **10 aggregate seconds requested through `sleep`**.

These are resource limits, not real-time deadlines for foreign calls or OS
scheduling. Ordinary type/arity failures are diagnosed during `--check`;
resource, copy, stale-context, and body failures are runtime `LanguageError`s.

## Tests

```sh
python3 cornerstone/LLSynth/tests/execution/test_execution.py
```

The suite includes lexical mutation/escape/shadowing, higher-order calls,
async returns and exceptions, task snapshots and result copying, a real
two-thread barrier test, structured joining, context isolation, resource
limits, and rejection on every native tool target.
