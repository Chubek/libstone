# Cephyr Lua extensions

Cephyr runs each request-supplied extension in a separate Lua state. A
script's top-level code registers its callbacks; its globals and closures
persist only for that compile. Native SDK registrations precede Lua
registrations. Lua hooks run in script and registration order within each
stage. A directive's exact target-triple selector takes precedence over a
selector that matches all targets.

```lua
local c = require("lcephyr")

c.on("desugar", "observe_c", function(ctx)
    if c.env.get("BUILD_MODE") == "checked" then
        c.report("note", "build.checked", "Checking " .. ctx.input_name)
    end
end)

c.directive("answer", function(ctx)
    local n = tonumber(ctx.text)
    if not n or math.type(n) ~= "integer" or n < 0 or n > 2147483647 then
        c.report("error", "answer.invalid", "Expected a nonnegative C int")
        return
    end
    c.define_integer("cephyr_answer", n)
end)
```

With that extension, a C source file can use:

```c
#pragma cephyr answer 42
int answer(void) { return cephyr_answer; }
```

Save the script as `build.lua` and load it explicitly:

```sh
cephyr --lua-extension build.lua --lua-env BUILD_MODE=checked -fsyntax-only answer.c
```

Both options can be repeated. `--lua-env` accepts only explicit `NAME=VALUE`
grants; it never reads a name from the process environment. Scripts loaded
with `--lua-extension` share those grants. Embedded callers can instead pass script
bytes and grants via [`cephyr_request.lua`](../EMBEDDING.md). Projects can
also [load scripts from `Cephyr.toml`](CONFIGURATION.md).
The driver accepts up to eight scripts of 1 MiB each and 64 environment
entries. Keys are ASCII identifiers, and values are at most 4 KiB each.

## API

- `c.env.get(name)` returns the explicitly supplied value or `nil`; an empty
  supplied value is `""`. `c.env.list()` returns a sorted array of the supplied
  names. Local edits to the array do not affect the allowlist. The compiler's
  process environment is not read by the extension runtime.
- `c.on(stage, name, callback [, target_triple])` registers a top-level
  observer. Stages are `"desugar"` (raw C), `"analyze"` (canonical MIR), and
  `"rewrite"` (MIR at that point in the SDK pipeline). Hook names are unique
  and contain only ASCII letters, digits, `_`, `-`, or `.`. A `nil` or empty
  selector matches all targets. Lua cannot replace hook text or register an
  assembly backend.
- `c.directive(name, callback [, target_triple])` registers a handler for
  `#pragma cephyr NAME ...`, `#pragma GCC NAME ...`, or either `_Pragma`
  equivalent. The name is an ASCII C identifier, and `ctx.text` is the
  unexpanded argument text. Only one handler can match a specific
  `(name, target_triple)` pair. An unclaimed `GCC` pragma does not call Lua.
- Each callback receives a read-only snapshot `ctx` with `stage`, `text`,
  `input_name`, `target_triple`, `target_architecture`, `target_abi`,
  `mir_schema_version`, and `flags`. A directive also has `directive_name`,
  `line`, and `column` (zero means unknown). Access the fields by name.
- `c.report(severity, code, message [, file [, line [, column]]])` is available
  inside callbacks. Severity is `"note"`, `"warning"`, or `"error"`. Codes
  have the same syntax as hook names. Omit `file` for the input path and use
  one-based coordinates, or zero/`nil` for unknown. For a directive, omitting
  the file and coordinates uses the pragma location.
- `c.define_integer(name, value)` may be used once during a directive
  callback. It inserts only a compiler-formatted C `enum { name = value };`
  declaration after preprocessing. The name must be an ASCII C identifier
  and the value must be a Lua integer in `0..2147483647`. There is no arbitrary
  text emission API.

Hook and directive return values are ignored. Report an `"error"` diagnostic
to reject compilation. A Lua runtime error also rejects compilation.

## Sandbox

No filesystem, process, or network functions are exposed. `require` only
returns the built-in `lcephyr` module. There is no `package`, `io`, `os`,
`debug`, `print`, `warn`, dynamic loading, coroutine, or Lua-side protected
call. Only text chunks are accepted. Scripts cannot create or inspect
metatables, or use `rawset`. `tostring` does not expose object addresses.

The curated libraries include:

- `string`: `byte`, `char`, `find`, `len`, `lower`, `rep`, `reverse`, `sub`,
  `upper`. `find` is literal-only, accepts an optional starting index, and
  limits the needle to 256 bytes. Patterns, binary dumping, and generic
  formatting are unavailable.
- `table`: `concat`, `insert`, `pack`, `remove`, `unpack`.
- `math`: `abs`, `ceil`, `floor`, `huge`, `max`, `maxinteger`, `min`,
  `mininteger`, `pi`, `tointeger`, `type`.

Each script has a 32 MiB live Lua allocation cap and a cumulative budget of
1 million Lua VM instructions, charged in blocks of 1,000 and with a minimum
charge per invocation. There is also a cumulative two-second execution budget
for loading and invoking each script; time spent in the compiler between
callbacks does not count. The clock is checked at VM instruction checkpoints
and after each protected invocation, so it is not an operating-system
preemption guarantee for an individual native library operation. Budget
exhaustion is a compilation error.

The sandbox constrains the extension script, not the compiler as a whole.
Compiler inputs, native SDK callbacks, target definitions, and build tools
retain their normal trust requirements.
