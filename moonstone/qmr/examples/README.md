# Moonstone QMR — Lua-backed template rendering

QMR is a C++20 template compiler and renderer whose **execution engine is the
vendored Lua VM** at `third_party/lua`, accessed through the vendored Kaguya
binding at `third_party/kaguya`. It does not use system Lua. The Lua VM is
instantiated afresh per render. This overlay fits in `libstone/moonstone/qmr`.

**Implementation status:** C++ API, template-to-Lua compiler, native Lua sink,
HTML escaping, include resolution, runtime limits, CLI, tests, sample templates
and standalone / opt-in parent CMake integration have been implemented.
The compiler test is runnable without Lua. Lua runtime tests require the
actual LibStone checkout and are not claimed to have passed in the isolated
ZIP build environment.

## Syntax

| Syntax | Meaning |
|---|---|
| `{{ data.name }}` | Evaluate a Lua expression, HTML-escape its text |
| `{{{ data.trusted_html }}}` | Evaluate an expression WITHOUT HTML escaping |
| `{% if condition then %}...{% else %}...{% end %}` | Native Lua control flow |
| `{% for _, x in ipairs(data.items) do %}...{% end %}` | Native Lua loop |
| `{% local x = data.value %}` | Native Lua statement |
| `{% include 'path.qmr' %}` | Include a template by relative path |
| `{% include 'path.qmr', {item=x} %}` | Include with `vars.item` set to `x` |
| `{# a comment #}` | Discard a comment |
| `{{- x -}}` | Trim adjacent template whitespace |

`data` is the explicit read model, created from the host C++ `Context`.
The `vars` global is an optional table supplied per include invocation.
Lua locals in a parent template are **not automatically inherited** by a
separately compiled include; pass them explicitly with the second argument.
Numeric, boolean, nil and string interpolation is supported; tables/functions
must be converted to a scalar explicitly (e.g. `table.concat`).

Functions exposed under `qmr`: `qmr.escape_html(s)`, `qmr.escape_url(s)`
and `qmr.default(value, fallback)`. Standard `math`, `string`, `table` and
`utf8` are available. Autoescaping affects only `{{ ... }}` and only the
HTML text-escaping rules; do not treat it as CSS, JavaScript or URL encoding.

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

## Building and testing

Extract the ZIP at **the LibStone repository root**, then run:

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

### CLI

```
qmr TEMPLATE [--root DIR] [--define KEY=VALUE] [--no-escape]
             [--max-output BYTES] [--check]
```

`--define` can be repeated; values are **strings**. Complex contexts should
use the C++ API. `--check` currently performs a dry-output render (it executes
Lua but suppresses output), so only use it with trusted templates. Includes
resolve within the canonical `--root` path; symlink escapes, absolute paths,
`..` traversal and include cycles are rejected. The default root is the main
template's parent directory.

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
`compile()` can be invoked independently to inspect generated Lua source.

## Limits and trust boundary

Each render runs in a fresh state and caps input bytes, generated code bytes,
output bytes, context size/depth, number of template nodes, include depth and
Lua instruction count. The Lua API's file I/O, package loader and code-loading
entry points are not registered; it is intentionally a reduced environment.
**These controls are NOT a security sandbox for attacker-supplied templates.**
The Lua VM is in-process, no Lua allocator memory quota is imposed, and Lua
code embedded in the template should be trusted. For hostile templates use
an OS-isolated process with memory and CPU constraints, plus a stricter
expression-only template language. `{{{ raw }}}` is an explicit HTML injection
risk unless the data has been independently validated.

## Source layout

- `include/qmr/Qmr.hpp`: public API
- `src/Compiler.cpp`: template lexer/compiler and escaping helpers
- `src/Engine.cpp`: Kaguya/Lua VM, evaluation and includes
- `tools/qmr.cpp`: command line renderer
- `tests/`: compiler + runtime + CLI tests
- `scripts/integrate_root.py`: opt-in Moonstone CMake integration
- `AGENTS.md`: expansion tasks and acceptance criteria
