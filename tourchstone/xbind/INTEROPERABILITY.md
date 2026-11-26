# Libstone native interoperability

The components describe different things. None infers a function signature from
an export name, a symbol table entry, or an address.

| Layer | Input | Output and boundary |
|---|---|---|
| Yellowstone qBFD / ArchBFD | Object or image bytes | Offline format, sections, symbols and mappings. qBFD supplies normalized image and linkage metadata to C++ consumers; ArchBFD is the separate stable application ABI. A linked virtual address, an RVA and a file offset are not interchangeable. |
| Tourchstone dynload | Path, inspection/loading policy, export selector | Offline inspection or a native module handle. Inspection neither runs constructors nor returns callable pointers. Native resolution requires a compatible shared library and a live handle; the operating system performs loading. |
| Tourchstone dyninvoke | Explicit signature, ABI, argument storage and native function address | ABI-specific layouts and, where supported, actual calls or closures. Layout success does not imply execution or closure support. Do not call addresses obtained from offline qBFD metadata. |
| XBind | Trusted Lua manifest, declaration headers and optionally an offline image | Deterministic spec IR, export inventory and manifest-to-export mapping, compiler signature assertions, and direct-link Lua 5.4 glue. The other eight language targets are registered scaffolds, not emitters. |

## XBind workflow

```sh
xbind --capabilities
xbind --discover libhello.so
xbind --bind-ir libhello.so /path/to/hello.xbind
xbind --emit-check /path/to/hello.xbind > check.cpp
c++ -std=c++20 -fsyntax-only -I/path/to/headers check.cpp
xbind --emit-lua /path/to/hello.xbind > hello_lua.cpp
```

`--discover` inventories imports, exports and needed libraries without loading
or executing the image; its output has no signature fields. Addresses, when
present, are object metadata, not process addresses. `--bind-ir` requires a
shared image and manifest declarations plus headers. It accepts the C name
as-is for ELF and PE and prepends `_` for Mach-O. A function's `symbol` option
selects an exact raw export name instead. For ELF, only an unversioned export or
the default version is matched unless `symbol_version` explicitly selects a
version. Missing or ambiguous exports fail without emitting a partial result.
For PE, `symbol_ordinal` selects a numeric export ordinal (1–65535) and is
mutually exclusive with name and version overrides.
Forwarders and reexports can appear in the mapping; the mapping does not resolve
them or prove that an export names code. An optional canonical qBFD architecture
name (e.g. `aarch64` or `x86-64`) constrains the image and is mandatory for
universal Mach-O. Unrecognized ABIs remain `unknown`.

The binding IR says `declared-unchecked` even when all export names match.
Compile the emitted assertions with the target compiler to check exact pointer
types against the actual headers, then link against the intended library. This
does not independently prove the library implements those declarations. The
export IR is a separate inspection artifact: the Lua backend compiles and links
against headers and rejects raw name/version/ordinal overrides rather than ignoring
them. It does not use dyninvoke, load an arbitrary binary by name, or support
callbacks, variadics, records by value, automatic pointer lifetimes or out
buffers. It supports the scalar subset, borrowed C strings and explicitly
mapped opaque pointers. Compile emitted glue with the Lua 5.4 SDK and the
DomLuaBind C API (`dombind/domluabind/generate.py`).
Integer-mapped C enum inputs require values from the C enum's documented domain;
the emitter does not derive an enum's valid values from its declaration.

## Build and embedding

```sh
cmake -S tourchstone -B build-interop -DLIBSTONE_BUILD_TESTS=ON
cmake --build build-interop --parallel
ctest --test-dir build-interop --output-on-failure
cmake --install build-interop --prefix "$PWD/stage"
```

Provide the vendored dependencies or set `LIBSTONE_THIRD_PARTY_DIR`. Discovery
defaults on when a `qbfd::qbfd` target is already present, as in the native
Tourchstone build. For standalone `tourchstone/xbind`, choose
`-DXBIND_BUILD_DISCOVERY=ON` and point `CMAKE_PREFIX_PATH` at an installed qBFD,
or leave it disabled for manifest-only functionality. `find_package(XBind
CONFIG)` exports `XBind::xbind` and `XBind::QaMRppSpec`; the latter exposes
the manifest, code generation and offline discovery APIs. `find_package(Tourchstone
CONFIG)` provides the enabled native libraries. Generated Lua glue needs no
XBind runtime library unless it is binding XBind itself.
Tourchstone installs the Sandstone `.isa` files used by dyninvoke when Sandstone
is not built with it. For a relocated prefix, pass its `share/sandstone/isa`
directory to `dinv_isa_regcounts` explicitly, or set `DINV_ISA_DIR`.
The full Yellowstone command suite is not required for this workflow:
Tourchstone brings in qBFD and ArchBFD on its own. Enable the full Yellowstone
build separately only with a working qobjld Aurocks generator.

## Verification and portability

The validation host for this work is Linux x86-64. These tests distinguish
offline evidence from actual execution:

| Test | What it verifies |
|---|---|
| `xbind.discovery` | Reproducible JSON from a versioned ELF shared object, PE DLL and thin/universal x86-64 and arm64 Mach-O. Exact match rules, ELF default and explicit versions, PE ordinals, Mach-O C spelling and explicit slice selection. Foreign fixtures are linked by Clang/LLD and inspected only. |
| `tourchstone.interop` | Match the hello manifest against a native shared image, check that inspection returns no callable address, load and resolve it natively, then pass explicit `i32` argument/result storage to dyninvoke. |
| `xbind.codegen`, `tourchstone.bindings` | Compile exact-signature assertions against headers. Compile and run generated hello, self, dynload and dyninvoke glue with Lua 5.4 and DomLuaBind. |
| `tourchstone.installed` | Install into a staging prefix, configure a separate C/C++ consumer via `find_package`, and run it and the installed XBind CLI. |

On Linux x86-64 with Clang 22.1.8, the integrated Tourchstone suite passed
24 tests in both the regular and AddressSanitizer/UndefinedBehaviorSanitizer
builds; one optional qBFD source-notes check skipped in each. Standalone
XBind also passed its six CTest entries with binary discovery disabled. A
top-level shared build scoped to Tourchstone and its bundled qBFD/ArchBFD
passed the same interoperability suite.

Native dyninvoke calls and closures are gated to Linux SysV AMD64. The
implemented offline layouts for Win64, AAPCS64, Darwin ARM64 and RISC-V hard
float (LP64D/ILP32D) do not imply native execution support. The cross-image
tests do not verify Windows or macOS native loading or ARM/RISC-V execution.
Missing optional cross toolchains or Lua 5.4 SDKs can skip corresponding cases.
A source assertion, export match or offline layout is not a substitute for
a host execution test.
