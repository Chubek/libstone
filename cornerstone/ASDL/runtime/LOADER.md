# Native ASDL Lua libraries

The native runtime installs `asdl.lib` and `asdl.signlib` on its existing `asdl`
table. Library files use this envelope:

```lua
LIB = asdl.lib.new("example.counter")
asdl.lib.version(LIB, "1.0")

local other = asdl.lib.require("example.base", "2.0")
function LIB.Next(n) return other.Start + n end

asdl.signlib(LIB)
return LIB
```

The first statement may instead be `LIB = asdl.lib.derive "example.base"`.
The name/path in the prologue must be one string literal. Whitespace, Lua
comments, either form of Lua string call, and semicolons between statements
are allowed. The final statements sign and return `LIB`. The loader parses
text Lua, checks the envelope, and checks that the chunk actually returned the
same signed table it created and assigned to the environment's `LIB`.
Signing seals the top-level string-keyed export map against regular Lua
assignment. It is not a cryptographic signature. Derivation makes a shallow
copy of the parent's exports, so replacing an inherited export leaves the
parent's binding unchanged, but table-valued exports can still share state.

## Resolution and metadata

- A new loader reads `LIBSTONE_ASDL_PATH` as a colon-separated list of
  directories, ignoring empty entries. Roots are searched left to right,
  followed by the installed stdlib root supplied by the host.
- `example.base` is `example/base.lua`. Names ending in `.lua` and names
  containing `/` are literal paths; a missing `.lua` suffix is appended
  for slash paths. An absolute path or one starting with `./` or `../`
  bypasses the roots and is relative to the process working directory when
  not absolute. Other relative paths are searched under the roots. Relative
  root entries also refer to the working directory, not the importing file.
- `asdl.lib.resolve(name_or_path)` returns the canonical filesystem path or
  raises an error. `asdl.lib.searchpath()` returns a fresh array of roots.
- Each loader caches successful loads by canonical path. Dotted names, file
  paths, and symlinks to the same resolved file share export identity.
  Dependencies may be loaded only after checking for cycles. A failed
  initializer is not cached; successfully completed dependencies can remain
  cached. There is no hot reload.
- `asdl.lib.loaded()` lists sorted canonical paths. `asdl.lib.issigned(lib)`
  tests whether a value is a signed table recognized by this loader.
  `asdl.lib.exports(lib)` lists sorted export names. `asdl.lib.describe(lib)`
  returns fresh metadata with `name`, `signed`, `exports`, `dependencies`
  (sorted unique direct canonical paths), and when available `path`, `parent`
  (canonical path), and `version`. Dependencies record successful `require`
  and `derive` calls during initialization, not later use of exported
  functions.
- `asdl.lib.version(lib, version)` declares an optional nonempty version string
  once, before signing. `asdl.lib.version(lib)` reads it. A second argument to
  `asdl.lib.require(name_or_path, version)` demands an exact match; undeclared
  versions do not match. Versions are opaque strings, and are not inherited
  from a derived library. There is no SemVer solver or package installer.

## Restricted environment and trust

A library runs with a new `_ENV` and its own `_G`, not the process Lua globals.
It gets selected base functions and separate copies of `math`, `string`,
`table`, `utf8` and the host `asdl` API (shallow copies of its table-valued
members). It does not get `io`, `os`, `package`, `debug`, ordinary Lua
`require`, `load`, `loadfile`, `dofile`, `rawset`, or coroutine controls.
Libraries use `asdl.lib.require` instead. The loader does not accept Lua
bytecode. The standard math random generator, referenced host state, and
values exported by dependencies can still be shared.

**This is capability reduction, not a security sandbox.** `asdl` functions
carry whatever authority the embedding process grants. Libraries can consume
unbounded CPU and runtime memory, modify shared objects, and use the ASDL
loader to resolve or attempt to load files. Explicit paths, symlink targets,
and root order are intentional and do not enforce directory containment.
Signing does not protect a table against debugging tools or trusted host code
using `rawset`. Load only trusted Lua code and use trusted roots (including
the environment variable, which can shadow stdlib). Use process-level
isolation for untrusted files. File lookup/open assumes the relevant
directories are not being changed by a hostile party.
