# 8. C/C++ embedding and bindings

## C++ construction

```cpp
#include <Flintstone.hpp>

flintstone::Registry registry;
flintstone::Module module;
module.name = "built";
module.level = flintstone::Level::Core;

flintstone::Function function;
function.name = "answer";
function.result = flintstone::Type::integer(32);
flintstone::Builder builder(function);
auto entry = builder.block("entry");
auto value = builder.emit(entry, "arith.add", flintstone::Type::integer(32),
    {flintstone::Operand::constant(20), flintstone::Operand::constant(22)});
builder.return_value(entry, flintstone::Operand::ref(value.id));
module.functions.push_back(std::move(function));

flintstone::require_valid(module, registry);
auto answer = flintstone::execute(module, registry, "answer");
```

The builder allocates stable result/block IDs and constructs operations; it
does not replace the verifier. For forward loop references or multi-result
operations, construct the owning records directly and verify after definitions
and CFG edges have been supplied.

`Module::function` and `Function::block` provide checked lookup. References into
vectors are subject to ordinary C++ invalidation rules after structural edits;
persist IDs rather than element addresses. `fresh_value` considers declared
parameters/results. Define each reserved identity exactly once before export.

`frontend/visitor.hpp` provides const and mutable walks over operations. It does
not recursively rewrite a vector while iterating it. Use a replacement list or
a pass for structural edits.

## C ABI

`Flintstone.h` is C11-compatible. `Flinestone.h` preserves the original
scaffold's misspelling as an include shim. Both headers can be included in the
same translation unit. Public functions use opaque handles and never expose
C++ STL objects or exceptions.

```c
#include <Flintstone.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char* source =
        "module m level core {func @f()->i32 {"
        "^e: %r:i32=arith.add(20:i32,22:i32);func.return(%r);}}";
    flintstone_context* context = flintstone_context_create();
    flintstone_module* module = NULL;
    int64_t answer = 0;
    if (!context) return 1;
    flintstone_status status = flintstone_parse(context, source, strlen(source),
                                               "embedded.flint", &module);
    if (status == FLINTSTONE_OK) status = flintstone_verify(context, module);
    if (status == FLINTSTONE_OK)
        status = flintstone_run_i64(context, module, "f", NULL, 0, 1000, &answer);
    if (status != FLINTSTONE_OK) fprintf(stderr, "%s\n", flintstone_last_error());
    flintstone_module_destroy(module);
    flintstone_context_destroy(context);
    return status == FLINTSTONE_OK && answer == 42 ? 0 : 1;
}
```

### API groups

| Function | Behavior |
| --- | --- |
| `flintstone_context_create/destroy` | Owns a builtin registry. |
| `flintstone_parse` | Length-delimited syntax parsing; sets output null on failure. |
| `flintstone_parse_text` | NUL-terminated convenience parser, owning handle or null. |
| `flintstone_module_destroy` | Releases an owning module handle. |
| `flintstone_verify` | Semantic verification with the context's registry. |
| `flintstone_lower` | Verified transactional conversion using a checked integer level code. |
| `flintstone_optimize` | Private high-to-core conversion if necessary, named Blackstone kernel and DCE; commits only on success. |
| `flintstone_print` / `flintstone_module_text` | Canonical text, borrowed from the module's print cache. |
| `flintstone_run_i64` | Integer-argument/result convenience execution with an explicit budget. |
| `flintstone_binding_target_count/at` | Tourchstone binding inventory. |
| `flintstone_version` / `flintstone_last_error` | Version and thread-local diagnostic text. |

The statuses are `FLINTSTONE_OK`, `FLINTSTONE_INVALID_ARGUMENT`,
`FLINTSTONE_ERROR` and `FLINTSTONE_OUT_OF_MEMORY`. Handle-returning convenience
functions return null on failure. Error text contains the first diagnostic's
code/location/context when available and may be truncated to the thread-local
error buffer. Copy it before another API call can replace it.

Parsing is deliberately separate from verification. A syntactically valid
module may still contain unknown operations or invalid SSA and must pass
`flintstone_verify` before use. Execution/lowering/optimization also verify
their own inputs.

### Ownership and lifetime

- Each create/parse owning handle has a corresponding destroy function.
- Null destroy arguments are safe.
- Modules own their IL and may outlive the context that parsed them. Another
  compatible context can verify/execute them.
- The text returned by printing is valid until the next successful print or
  mutation of that module, or its destruction.
- Binding target strings are borrowed static XBind inventory strings.
- Failed conversion/optimization does not replace the original module.
- Failed integer execution does not write a partial result.

The integer convenience API accepts `i1` arguments as int64 zero/one and returns
Boolean results as zero/one. Float/pointer/aggregate execution and custom
registry callbacks are C++ surfaces; the C API does not silently coerce them.

## Tourchstone XBind

`Flintstone.xbind` is the authoritative binding manifest for the supported
scalar/string/opaque subset. It names the nine existing targets: D, Go, Java,
Lua, NodeJS, OCaml, Python, Ruby and Rust.

```sh
/tmp/opencode/flintstone-build/dependencies/xbind/xbind \
  --check-spec Flintstone flintstone
/tmp/opencode/flintstone-build/dependencies/xbind/xbind \
  --emit-check Flintstone flintstone
/tmp/opencode/flintstone-build/dependencies/xbind/xbind \
  --emit-lua Flintstone flintstone
```

The spec includes context/module lifecycle, string parsing, verification,
lowering, text printing and target discovery. It deliberately omits the C
argument-buffer and output-pointer APIs, which the current emitter cannot
marshal automatically.

Only Tourchstone's Lua emitter is implemented. The other eight names are target
inventories, **not** a claim of generated executable bindings for all languages.
The Flintstone binding test generates the real DomLuaBind C API through its
existing generator, compiles the generated Lua glue, embeds the vendored Lua
runtime, and executes parse/verify/lower/print/lifecycle calls. C++ generated
signature assertions are also compiled against `Flintstone.h`.

Example Lua use after registering/loading the generated module:

```lua
local f = require('flintstone')
local context = assert(f.flintstone_context_create())
local module = assert(f.flintstone_parse_text(context, [[
  module m level high {
    func @f(%x:i32)->i32 { ^e: %r:i32=math.square(%x);func.return(%r); }
  }
]]))
assert(f.flintstone_verify(context, module) == 0)
assert(f.flintstone_lower(context, module, 1) == 0) -- FLINTSTONE_CORE
print(f.flintstone_module_text(module))
f.flintstone_module_destroy(module)
f.flintstone_context_destroy(context)
```

Opaque-handle ownership is explicit; do not destroy a handle twice or use it
after destruction. Do not assume the current generated glue supplies automatic
garbage collection for owning C handles.

## Concurrency

Independent modules, contexts, registries, runtimes and environments can be used
independently. Synchronize access to shared mutable objects and stateful host
callbacks. A pass run creates its own analysis cache; direct users of
`AnalysisManager` must invalidate it after changing the analyzed module. Do not
reuse a populated cache for a different module without invalidation.

Blackstone runtimes are created privately for each pass invocation; they are
not concurrently shared Lua states. C diagnostics are thread-local. Runtime
effects and callback state still belong to the application's synchronization
and transaction model.

Next: [Testing and limitations](09-testing.md).
