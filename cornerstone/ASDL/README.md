# ASDL: schema compiler and syntax-tree runtime

`asdlc` is the native schema compiler. It checks both legacy and
[extended ASDL](GRAMMAR.md), generates C11 interfaces, and can also emit a
standalone C++20 AST/reflection header or Graphviz DOT. `asdl-run` loads a schema
into a native, typed Lua AST session and executes a driver script against it.

```sh
cmake -S cornerstone/ASDL -B build/asdl
cmake --build build/asdl
ctest --test-dir build/asdl --output-on-failure
cmake --install build/asdl --prefix /path/to/prefix
```

Building from source uses the repository's `third_party/lua` (Lua 5.5.0) and
`third_party/kaguya` snapshots. It does not require a system Lua installation.
The native build needs a C++20 compiler and a C11 compiler. Perl and Python
enable additional regression tests but are not required to build the tools.
To embed the installed C/C++ library, use `find_package(ASDL CONFIG REQUIRED)`
and link `ASDL::runtime`. See [the embedding guide](embedding.md).

### Using the native tools

```sh
build/asdl/asdlc --header bootstrap.h --cpp-header bootstrap.hpp \
  cornerstone/ASDL/examples/Bootstrap.asdl
build/asdl/asdlc --check --dot schema.dot --dot-view dependencies \
  cornerstone/ASDL/examples/Bootstrap.asdl
build/asdl/asdl-run cornerstone/ASDL/tests/fixtures/calculator.asdl - <<'LUA'
assert(type(asdl.lib.require("std.tree")) == "table")
LUA
```

`asdl-run [-I DIR] [--stdlib DIR] SCHEMA SCRIPT [ARGS...]` runs a Lua source
file or reads it from standard input when `SCRIPT` is `-`. Driver arguments
are available in the one-based Lua `arg` table. `--check SCHEMA [SCRIPT]`
loads the schema and prepares its hooks; it syntax-checks an optional driver
without running that driver. The runtime locates its installed standard
library relative to the executable, or accepts `--stdlib` pointing to a
directory containing `std/`. The library loader also accepts
`LIBSTONE_ASDL_PATH` for additional module roots; see the
[loader reference](runtime/LOADER.md).

Only run trusted schemas, scripts, and module libraries. Library loading may
execute import code even in `asdl-run --check`. The runtime limits some Lua
capabilities but does not isolate untrusted code from the host process.

### Migrating an existing build

Existing consumers can keep the `scripts/asdl.pl` entry point unchanged. To
try the native compiler, first replace the `perl scripts/asdl.pl` invocation
with `asdlc`, keeping the schema and flags. Check the output format and
compile your own consumer against the result before switching. A native build
can use the `asdlc` target in a custom command, or the imported `ASDL::asdlc`
target from the installed package. When cross-compiling, use a separately
built host `asdlc` for generation.

The downstream regression compares JSON metadata and exercises the original
generation flags for consumers available in the checkout. It is not a claim
that generated C files are byte-for-byte identical or that all downstream
projects have been migrated.

## Legacy Perl compiler

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
The Perl generator does not implement ASDL pickling/serialization or module
imports.

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
