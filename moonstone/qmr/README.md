# Moonstone QMR — Lua-backed template rendering

QMR is a C++20 template compiler and renderer whose **execution engine is the
vendored Lua VM** at `third_party/lua`, accessed through the vendored Kaguya
binding at `third_party/kaguya`. It does not use system Lua. The Lua VM is
instantiated afresh per render. This overlay fits in `libstone/moonstone/qmr`.

**Implementation status:** C++ API, template-to-Lua compiler, native Lua sink,
HTML/XML/JSON/plain-text codecs, trusted `SafeHtml`, include resolution, source
maps for diagnostics, runtime limits (output, instructions, context, includes,
Lua VM memory, wall-clock deadline), compiled-template cache, CLI with strict
JSON data import, versioned C ABI, tests (compiler, runtime, lexer
fuzz/property, C ABI, CLI), standalone / opt-in parent CMake integration and a
sanitizer verification script have been implemented and pass on GCC and Clang
(Debug/Release) and under AddressSanitizer/UndefinedBehaviorSanitizer.

## Syntax

| Syntax | Meaning |
|---|---|
| `{{ data.name }}` | Evaluate a Lua expression, escape per the active codec |
| `{{{ data.trusted_html }}}` | Evaluate an expression WITHOUT any escaping |
| `{% if condition then %}...{% else %}...{% end %}` | Native Lua control flow |
| `{% for _, x in ipairs(data.items) do %}...{% end %}` | Native Lua loop |
| `{% local x = data.value %}` | Native Lua statement |
| `{% include 'path.qmr' %}` | Include a template by relative path |
| `{% include 'path.qmr', {item=x} %}` | Include with `vars.item` set to `x` |
| `{# a comment #}` | Discard a comment |
| `{{- x -}}` | Trim adjacent template whitespace |

Inside tag bodies the lexer honors Lua's own quoting rules: single/double
quoted strings (with escapes), long-bracket strings (`[[...]]`, `[==[...]==]`),
line comments and long comments (`--[[...]]`, `--[==[...]==]`) — a QMR
delimiter inside such a construct is data, not a delimiter. An unterminated
string or comment inside a tag is rejected with `QMR_UNCLOSED`.

`data` is the explicit read model, created from the host C++ `Context`.
The `vars` global is an optional table supplied per include invocation.
Lua locals in a parent template are **not automatically inherited** by a
separately compiled include; pass them explicitly with the second argument.
Numeric, boolean, nil and string interpolation is supported; tables must be
converted to a scalar explicitly (e.g. `table.concat`).

Functions exposed under `qmr`: `qmr.escape_html(s)`, `qmr.escape_url(s)`,
`qmr.default(value, fallback)` and `qmr.safe_html(s)`. Standard `math`,
`string`, `table` and `utf8` are available.

### A sample

```qmr
<h1>{{ data.title }}</h1>
<ul>
{% for _, item in ipairs(data.items) do %}
  {% include 'item.qmr', {item=item} %}
{% end %}
</ul>
```

`item.qmr`:

```qmr
<li>{{ vars.item }}</li>
```

## Escaping codecs and SafeHtml

`Options::codec` selects the escaping applied to `{{ ... }}`:

| Codec | Rule for `{{ x }}` |
|---|---|
| `Html` (default) | `& < > " '` become entities |
| `Xml` | `& < > " '` become the XML predefined entities (no XML 1.0 character-legality validation) |
| `Json` | strings become complete quoted JSON string literals (incl. `\u00XX` for controls); numbers/booleans pass through; nil becomes `null` |
| `Plain` | no escaping at all |

`{{{ ... }}}` is never escaped. **None of the codecs validates the surrounding
document.**

`SafeHtml` is the explicit trust marker for pre-escaped markup. A `SafeHtml`
value is emitted raw under the HTML and plain codecs — even through
`{{ ... }}` — so validated markup is not double-escaped. It is *not* trusted
for other formats and stays escaped under XML/JSON. In C++ use
`Value(SafeHtml{"<b>x</b>"})`; in a trusted template use
`qmr.safe_html(s)`. Plain strings remain escaped by default; making a value
trusted is always an explicit act by the host or the template author.

## Limits and trust boundary

Each render runs in a fresh Lua state and is bounded by `Limits`:
input bytes, generated-code bytes, output bytes, context size/depth, template
node count, include depth, Lua instruction count, **Lua VM allocation quota**
(`max_memory_bytes`, enforced by a custom allocator), and a **wall-clock
deadline** (`max_duration`, checked from the VM instruction hook). Allocation
failure paths surface as ordinary `QMR_EVALUATE` memory errors, and a blown
deadline raises `QMR_DEADLINE`.

The Lua API's file I/O, package loader and code-loading entry points are not
registered; it is intentionally a reduced environment.
**These controls are NOT a security sandbox for attacker-supplied templates.**
Lua code embedded in the template should be trusted. For hostile templates use
an OS-isolated process with memory and CPU constraints, plus a stricter
expression-only template language. `{{{ raw }}}` is an explicit HTML injection
risk unless the data has been independently validated.

### Diagnostics and source maps

The compiler emits a line-level map from generated Lua back to template
line/column (`Program::line_map`, `Program::locate_lua_line`). Runtime and
compile diagnostics from the main template *and from includes* are rewritten to
`file:line:col: QMR_CODE: message`, preserving the included template's
original position.

### Compiled-template cache

`ProgramCache` (share it across engines via `Options::cache`) keys entries on
the exact `(name, source text)` pair — a modified source is a different entry,
which is the invalidation rule. A byte budget evicts the oldest entries first;
the cache is thread-safe and can back concurrent renders. File modification
tracking is deliberately absent until tested; callers invalidate explicitly
with `invalidate()`/`clear()`.

## Building and testing

At the **LibStone repository root**:

```sh
# Standalone target (does not require editing the parent build):
cmake -S moonstone/qmr -B build-qmr -DCMAKE_BUILD_TYPE=Debug \
  -DQMR_BUILD_TESTS=ON -DQMR_BUILD_TOOLS=ON
cmake --build build-qmr --parallel
ctest --test-dir build-qmr --output-on-failure
./build-qmr/qmr moonstone/qmr/examples/greeting.qmr --define name=QMR

# Alternatively, register QMR under Moonstone's opt-in build:
python3 moonstone/qmr/scripts/integrate_root.py
cmake -S moonstone -B build-moonstone -DMOONSTONE_BUILD_QMR=ON
cmake --build build-moonstone --target qmr qmr_runtime_tests
```

The parent integration script makes only an idempotent modification to
`moonstone/CMakeLists.txt`, preserving the other Moonstone targets. It adds
`MOONSTONE_BUILD_QMR=OFF` so the dependency cost is opt-in.

If `moonstone_prolific_lua_vendor` is present in the parent build, QMR reuses
that vendored Lua library instead of building a second copy. The QMR-only
build builds the original vendor C sources. **Do not link separate Lua VM
copies into the same final executable** if you bypass this integration.

### Full verification matrix

`sh moonstone/qmr/scripts/verify.sh` runs: GCC and Clang, Debug and Release,
a full ctest each; a sanitized (ASan/UBSan) build with all tests; and the root
integration. `-DQMR_SANITIZE=ON` adds `-fsanitize=address,undefined` to QMR
targets and tests for sanitizer-in-CI use.

### CLI

```
qmr TEMPLATE [--root DIR] [--define KEY=VALUE] [--data FILE.json]
             [--codec html|plain|json|xml] [--no-escape]
             [--max-output BYTES] [--check]
```

`--define` can be repeated; values are **strings** and win over `--data`.
`--data FILE.json` imports strict RFC 8259 JSON (parsed with the vendored
klib `kson`; a contract validator runs first) — the document root must be an
object, nesting/bytes are bounded by `Limits`, and duplicate keys, trailing
content and non-standard extensions are rejected. JSON import lives only in
the CLI; the core library has no JSON dependency. `--check` performs a
dry-output render (it executes Lua but suppresses output), so only use it with
trusted templates. Includes resolve within the canonical `--root` path;
symlink escapes, absolute paths, `..` traversal and include cycles are
rejected. The default root is the main template's parent directory.

### C++ embedding

```cpp
#include <qmr/Qmr.hpp>
using namespace moonstone::qmr;
int main() {
    Context data{
        {"title", "Teams & Members"},
        {"items", Value::Array{"Ada", "Grace", "Ken"}},
    };
    Engine engine;
    auto html = engine.render_file("templates/layout.qmr", data, "templates");
}
```

The host can supply any `Resolver` to `Options::resolver` for virtual template
sources. It must provide a stable source identity to enable cycle detection.
`compile()` can be invoked independently to inspect generated Lua source and
its line map.

### C ABI

`include/qmr/qmr_abi.h` exposes a versioned C interface (`QMR_ABI_VERSION 1`):
`qmr_engine_create/destroy`, `qmr_engine_render[_file]` with flat string
key/value contexts, error buffers released with `qmr_free`, and
`qmr_abi_version()` for the compatibility check. The ABI test is compiled as C
to keep the header honest. There is no Lua-callable host module: nothing has
demonstrated demand for one.

## Source layout

- `include/qmr/Qmr.hpp`: public API
- `include/qmr/qmr_abi.h`: versioned C ABI
- `src/Compiler.cpp`: template lexer/compiler, source maps, codec escapers
- `src/Engine.cpp`: Kaguya/Lua VM, bounded allocator, deadline, includes,
  cache, SafeHtml runtime, diagnostics mapping
- `src/Abi.cpp`: C ABI implementation
- `tools/`: CLI (`qmr.cpp`) and strict JSON import (`json_import.*`, klib kson)
- `tests/`: compiler, runtime (incl. concurrency, symlink escapes, limits),
  lexer fuzz/property, C ABI, plus CLI tests via CTest
- `scripts/integrate_root.py`: opt-in Moonstone CMake integration
- `scripts/verify.sh`: sanitizer + multi-compiler verification matrix
- `AGENTS.md`: expansion tasks and acceptance criteria
