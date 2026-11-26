# ASDL: generate C11 syntax trees from a schema

[`scripts/asdl.pl`](scripts/asdl.pl) is a standalone compiler for the Zephyr
Abstract Syntax Description Language (ASDL). It needs Perl 5.14 or newer and
only core Perl modules. This is the canonical copy of the repository's
`asdl.pl` generator; it preserves the existing CLI, schema format, and
generated C interfaces.

## Quick start

From this directory, save the following as `tree.asdl`:

```asdl
module Tree {
    span = (int start, int end)
    expr = Number(int value)
         | Add(expr left, expr right)
         | Block(expr+ items)
         attributes (span? location)
}
```

```sh
perl scripts/asdl.pl --header tree.h --visitor walk.h tree.asdl
perl scripts/asdl.pl --check --schema tree.json tree.asdl
```

The first command writes a header-only AST interface and an optional visitor
template. Include `tree.h` and initialize a `tree_arena` with
`tree_arena_init(&arena)`. Construct a leaf with
`tree_Number(&arena, 42, tree_span_none())` and check the returned pointer
before using it. `tree_arena_destroy(&arena)` frees all arena-owned storage.
The second command validates the schema and exports JSON field names, C
spellings, constructor tags, and source locations without generating C.

To compile the AST implementation separately, generate it with
`--header tree.h --source tree.c` and compile `tree.c` as C11. The default
header goes to stdout if `--header` is omitted; `-` as the input name reads
stdin. For a separate shared runtime header, use `--builtins-header FILE`.
`--no-builtins` omits the shared runtime; schemas that reference its types
still need matching runtime declarations and operations. The extended runtime
and optional `--listener FILE` use POSIX facilities.

Products have named fields; sums have tagged constructors and can carry
attributes shared by every constructor. `?`, `*`, and `+` mean optional,
zero-or-more, and one-or-more respectively. Recursive sums are supported;
recursive products are rejected. Runtime containers passed into builders are
shallow copies; keep their owning storage alive while the tree uses it.

Run `perl scripts/asdl.pl --help` for a CLI summary or
`perl scripts/asdl.pl --man` for grammar details, C API and lifetime rules,
macro-configured visitors, runtime components, and listener examples.
The generator does not implement ASDL pickling/serialization or module imports.

## Tests

```sh
perl -c scripts/asdl.pl
prove -v tests/asdl.t
```

The suite checks local `.asdl` fixtures and, when the rest of the repository is
available, schemas used by the linker and TraceML. It verifies malformed-input
locations and diagnostics, output preservation, CLI overrides, and schema
metadata. With a C11 compiler on a POSIX host, it builds and runs both
header-only and split output across multiple translation units, exercising
arena ownership, validation, visitors, typed containers, and file events.
Set `CC` to select a compiler and `CFLAGS` for extra flags.

The standalone CMake project also registers the legacy suite and checks the
generation recipes for downstream schemas available in the checkout:

```sh
cmake -S cornerstone/ASDL -B build/asdl
cmake --build build/asdl
ctest --test-dir build/asdl --output-on-failure
```

Libstone's top-level CMake can include the ASDL project through
`-DLIBSTONE_BUILD_ASDL=ON`. The root option does not change any existing
consumer's use of `scripts/asdl.pl`.

### Developer transport

On platforms with linkable POSIX message queues the build includes `mqctl`.
It is a development tool, not part of the schema runtime or installed package.
Disable it with `-DASDL_BUILD_MQCTL=OFF`. The transport test uses a disposable
queue with a randomly generated name, not any live coordination queues.

`mqctl create SESSION ROLE` creates an exclusive queue, `inspect` reports
capacity and pending messages, and `cleanup` removes the name. `send` accepts
a payload on standard input; `receive` prints the validated `ASDL-MQ/1` header
and the payload bytes. Timeouts return status 3, invalid requests return 2,
and other errors return 1.
