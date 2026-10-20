# 10. Standard library and imports

Flintstone ships a standard library of ordinary IL modules in
`flintstone/stdlib/`. A module imports a library with an `import`
declaration; the resolver links the library's definitions into the importing
module and replaces matching `extern` declarations.

## The `import` declaration

```text
module app level core {
  import "std";
  extern func @std.math.gcd_i32(%a:i32, %b:i32) -> i32;
  func @compute(%a:i32, %b:i32) -> i32 {
    ^entry:
      %g:i32 = func.call(%a, %b) attrs { callee = "std.math.gcd_i32"; };
      func.return(%g);
  }
}
```

`import "name";` takes a plain library name, not a path. The name resolves to
`<name>.flint` in the search directories. The declaration is part of the
module's canonical text, so `print`/`parse` round-trip preserves it.

A library may declare the functions it provides as `extern` or define them
directly. When the host declares the same symbol as an `extern`, the resolver
requires identical signatures and replaces the declaration with the library's
definition. A host that *defines* a symbol the library also defines is a
conflict (`import.duplicate`), never a silent override.

## Resolution

```cpp
flintstone::resolve_imports(module, registry);                       // default search path
flintstone::resolve_imports(module, registry, {"/opt/my/libs"});      // explicit directories
```

The C ABI exposes the same operation:

```c
flintstone_status flintstone_resolve_imports(flintstone_context*, flintstone_module*, const char* stdlib_dir);
```

Passing `NULL` for `stdlib_dir` uses the default location. The CLI resolves
imports automatically and accepts extra directories with `-I DIR` or
`--stdlib DIR` (repeatable).

`verify()` rejects a module that still has unresolved imports
(`import.unresolved`), so importing is an explicit step, never an implicit
side effect of compilation.

### Search path

1. Directories passed explicitly to `resolve_imports` (in order).
2. `FLINTSTONE_STDLIB_DIR` from the environment, when set.
3. The default standard library directory.

The default directory is resolved from the loaded shared library's own path
via `dladdr`, giving `<prefix>/share/flintstone/stdlib` for an installed
package. This keeps a relocated prefix self-contained: no compiled-in
install path and no environment variable are required. When the library
cannot determine its own location (for example a statically linked host), the
build-time source directory is used as the fallback.

## Contracts

| Diagnostic | Meaning |
| --- | --- |
| `import.name` | The name is not a valid library name, or contains `..`. |
| `import.not_found` | No `<name>.flint` exists in the search path. |
| `import.cycle` | The library imports itself, directly or transitively. |
| `import.depth` | Import nesting exceeded 32 levels. |
| `import.signature` | Host and library declare the same symbol with different types. |
| `import.duplicate` | Both host and library define the same symbol. |
| `import.target` | Target name, pointer width, index width or byte order disagree. |
| `import.feature` | The library requires a target feature the host does not enable. |
| `import.dialect` | The library and host configure the same dialect differently. |
| `import.level` | The library is at a higher IL level than the host module. |
| `import.limit` | The library exceeds the import size budget. |
| `import.unresolved` | `verify()` ran before `resolve_imports()`. |

Library names are validated before any filesystem access, so `..` and path
separators cannot escape the search directories. A library is parsed, its
transitive imports are resolved first, and it is then specialized and
verified on its own before it is merged into the host.

Resolution is transactional: a failed import leaves the host module
unchanged. A successful resolution advances the module revision.

## Shipped libraries

| Library | Contents |
| --- | --- |
| `math` | `min_i32`, `max_i32`, `clamp_i32`, `abs_i32`, `sign_i32`, `gcd_i32`, `lcm_i32`, `pow_i32`, `factorial_i32`, `fib_i32`, `isqrt_i32` |
| `bit` | `popcount_i32`, `ctz_i32`, `clz_i32`, `is_pow2_i32`, `bswap_i32` |
| `fp` | `abs_f64`, `min_f64`, `max_f64`, `clamp_f64`, `lerp_f64` |
| `mem` | `swap_i32`, `fetch_add_i32` over `ptr<generic>` heap cells |
| `std` | Umbrella module importing `math`, `bit`, `fp` and `mem` |

The `math` and `bit` libraries are ordinary Core-level IL: loops, phis and
modular integer arithmetic, with no host intrinsics. They lower, optimize and
execute through exactly the same pipeline as user code.

`mem` operates on the VM's checked heap cells. `ptr<generic>` values are VM
addresses, not native pointers.

## Using the standard library

```sh
flintstone --run compute --arg 42 --arg 56 flintstone/examples/stdlib_demo.flint
# 17
```

```cpp
auto module = flintstone::parse(source);
flintstone::resolve_imports(module, registry);
flintstone::require_valid(module, registry);
auto answer = flintstone::execute(module, registry, "compute",
                                  {std::int64_t(42), std::int64_t(56)});
```

A module that imports a library inherits the library's target requirements:
the library's target layout must match the host's, and every feature the
library needs must be enabled by the host. This keeps a library from silently
changing the machine the host was compiled for.

## Writing a library

A library is a normal Flintstone module:

```text
module mylib level core {
  target "vm" { pointer_bits = 64; index_bits = 64; endian = "little"; }
  dialect arith { overflow = "modular"; }
  func @mylib.double(%x:i32) -> i32 {
    ^entry:
      %r:i32 = arith.add(%x, %x);
      func.return(%r);
  }
}
```

Place it in a search directory and `import "mylib";`. Library names are
global identifiers, so use a dotted or prefixed name such as `mylib.double`
to avoid collisions with other libraries.

Libraries are ordinary modules: they are verified, lowered, optimized and
executed like any other code, and a library may itself import other
libraries.

Next: [Diagnostics and testing](09-testing.md).
