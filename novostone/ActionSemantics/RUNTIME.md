# Portable Action Machine

Both dialect modes emit standalone immutable tables plus the same target-native
machine from `runtime.{c,py,rs,pas}.in`.
Runtime tags are integer (0), token (1), cell (2), closure (3), continuation (4),
and generator (5). No floats, big integers, user data algebras or objects exist yet.
Arithmetic is checked signed i32, without coercion. Function/continuation/generator
values print `closure`, `continuation`, `generator`, without exposing heap indices.

Environments and transient tuples are GC objects. Closures retain defining syntax
and lexical environments. Saved frames retain all lexical/transient/intermediate
roots. Cells can form cycles with closures and generators. Mark-sweep traverses
these edges without recursive host marking. Cells are monotonically allocated
until interpreter exit; there is no `free`, weak reference or alternate memory
policy. Live cell contents are roots even if their cell handle becomes unreachable.

Outcomes: completed, failed, escaped; generator suspension is an internal fourth
control outcome delivered only to `next`. Resource errors are terminal and cannot
be caught by `otherwise` or `trapping`. Exit codes remain 0, 1, 2 as in README.
Return/break/continue are explicit frame transfers, not language exceptions.

## Budgets

CLI syntax: `--dialect 2 --define KEY=POSITIVE_INTEGER` before `--target`.
The library `Options` uses zero for defaults and rejects negatives. Overrides on
dialect 1 are rejected. All backends enforce the same configured budgets.

| Key | Default | Maximum override |
| --- | ---: | ---: |
| steps | 100000 | 2147483647 |
| nodes | 2048 | 65536 |
| symbols | 256 | 4096 |
| refs | 1024 | 65536 |
| depth | 128 | 4096 |
| heap | 4096 | 65536 |

`heap` counts objects, not bytes: tuple/map objects have bounded 64-entry payloads
and captured continuations consume one object per saved frame plus their header.
These caps bound generated arrays and prevent overflow or unbounded configuration
allocation. They are safety caps, not performance promises.
Source bytes (65536), token bytes (64), parse depth (128), tuple/map length (64),
constructors (256), fields (16), lowered instructions (8192) remain fixed.
Symbols cannot be budgeted below the specification's literal token table.

In extended mode, each dispatch and yielder evaluation consumes steps. Legacy
mode charges only semantic action entries and yielders, retaining logical depth
through tail-replaced `evaluate` frames. This preserves the old accounting
without retaining a second interpreter. Tail operations avoid unbounded
action-frame growth; divergence eventually exhausts steps. Exhaustion is not a
proof of divergence.
Host parsing/yielder recursion and OS allocation behavior remain backend-specific;
configured resource limits, rather than arbitrary OS OOM recovery, are tested.
