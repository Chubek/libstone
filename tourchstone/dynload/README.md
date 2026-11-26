# Tourchstone dynload

`tourchstone/dynload` is a native dynamic-loader wrapper with qBFD-backed object
inspection. It is intended to cover the small-loader use cases of Dynalo and
libltdl while also exposing metadata those libraries do not model: static and
dynamic symbol tables, sections, relocations, dependencies, address-to-file
mapping, and a promoted-symbol environment handle.

## Build

```sh
cmake -S libstone/tourchstone/dynload -B build-dynload
cmake --build build-dynload --parallel
ctest --test-dir build-dynload --output-on-failure
```

The aggregate Tourchstone build enables dynload by default:

```sh
cmake -S libstone/tourchstone -B build-tourchstone \
  -DTOURCHSTONE_BUILD_DYNLOAD=ON
```

## C API sketch

```c
tdyn_env_t *env = NULL;
tdyn_library_t *lib = NULL;
void *sym = NULL;

tdyn_env_create(&env);
tdyn_env_open(env, "libplugin.so", TDYN_OPEN_NOW | TDYN_OPEN_LOCAL, &lib);
tdyn_library_symbol(lib, "plugin_init", &sym);
tdyn_env_promote(env, lib, TDYN_SYMBOL_TABLE_DYNAMIC);
tdyn_env_symbol(env, "plugin_init", &sym, NULL);
tdyn_env_destroy(env);
```

`tdyn_library_*_info` functions return qBFD-derived metadata. Returned strings
are borrowed from the library handle and remain valid until it is closed or its
owning environment is destroyed.

`tdyn_env_new` and `tdyn_library_load` are pointer-return convenience functions
for bindings: NULL signals failure and `tdyn_last_error` provides the diagnostic.
Close standalone libraries once with `tdyn_library_close`. Closing an
environment-owned library removes it and its promoted symbols; all aliases and
metadata pointers become invalid. Promotion requires membership in that same
environment, preventing stale symbols from a separately owned library.

## Inspection vs. loading

`TDYN_OPEN_INSPECT_ONLY` requires successful qBFD inspection and never asks the
host loader to load the file. Use it for foreign images, relocatable objects, or
any image whose code you do not intend to execute. For a native open, a known
format, architecture, address-width, byte-order, or image-kind mismatch returns
`TDYN_UNSUPPORTED`; the open does not silently become offline. The compatibility
query applies only to these preliminary checks, not to dependencies, detailed
platform ABI requirements, or whether the OS will accept the image. The ABI
name may be `unknown` even when the image is loadable. Native loads may run
constructors.

`TDYN_OPEN_REQUIRE_INSPECT` inspects a resolved path before native loading and
fails if no file can be inspected. Without it, an unresolved name may fall
back to native search. The loader may find an image with no inspectable path,
particularly on systems with a shared image cache. `tdyn_env_resolve_library`
only probes the given name in the current directory and then the registered
search directories in order (with platform filename suffix candidates for
unqualified names). It does not resolve the transitive dependency graph or
change the native loader search path. `tdyn_library_needed` exposes declared
dependencies, not a search result. Native failures preserve the loader's error
and can include declared dependency names for context.

`tdyn_library_link_symbol_count` and `tdyn_library_link_symbol_info` provide
normalized imports and exports, including available ELF versions, PE ordinal
and forwarding metadata, and Mach-O linker spelling. Linked VAs and optional
ordinals have explicit presence flags. On-disk addresses are not callable.
`tdyn_library_file_offset_for_address`, `tdyn_library_file_offset_for_rva`,
and `tdyn_library_address_for_file_offset` translate file-backed addresses;
RVA is relative to the linked image base and offsets are relative to the
selected Mach-O slice, not the containing universal file.
`tdyn_library_runtime_address` is a separate numeric mapping for loaded images
on Windows and ELF loaders exposing program headers through `dlinfo`. It is
not symbol lookup and returns `TDYN_UNSUPPORTED` if native mapping information
is unavailable (including Mach-O).

Resolve executable symbols with `tdyn_library_symbol` (native spelling,
including unprefixed C symbol names on Mach-O),
`tdyn_library_symbol_version` (`dlvsym` where detected), or
`tdyn_library_symbol_ordinal` (Windows `GetProcAddress`). Ordinal lookup
requires inspected PE export metadata to reject sparse export table holes.
The native loader resolves forwarded exports, not the on-disk forwarder
string. Failed symbol lookups clear outputs.

Promotion registers named, default-version exports for native lookup. Selecting
the static table restricts promotion to names also defined in that table; it
does not make private symbols callable. The address is resolved on each lookup
so thread-local addresses are not cached across threads. The reported owner is
the searched handle, not necessarily the image that defines a forwarded or
dependency symbol. Keep handles alive while using their addresses; synchronize
mutations and close against concurrent use.

Unknown flags and mutually exclusive lazy/now or local/global flags are rejected.
Windows does not implement lazy, global, or deepbind modes. POSIX nodelete handles
still release their acquired loader reference; Windows nodelete pins the module
explicitly.

## Tool

`tdynload` opens a library, inspects it, and resolves symbols:

```sh
tdynload --inspect-only --inspect --needed ./libplugin.so
tdynload --manifest ./plugin.dll
tdynload --inspect-only --exports --imports ./libplugin.so
tdynload --promote --env-symbol plugin_init ./libplugin.so
tdynload --resolve-version plugin_init@PLUGIN_1 ./libplugin.so
tdynload --resolve-ordinal 17 ./plugin.dll
tdynload --self --resolve malloc
```

Use `-L DIR` to add search paths and `--default-paths` to import the platform
path environment (`LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`, or `PATH`) after
explicit `-L` entries. `--manifest` implies inspect-only and emits a
`tdynload-inspection-v1` JSON document. Addresses are hex strings; absent link
symbol addresses and ordinals are `null`. Metadata byte strings are escaped
byte-for-byte.

## XBind spec

`DynLOAD.xbind` describes the string/int/handle query subset for all XBind
backends: D, Go, Java, Lua, NodeJS, OCaml, Python, Ruby, and Rust. APIs that
return record structs (`tdyn_symbol_info_t`, `tdyn_section_info_t`,
`tdyn_relocation_info_t`) or require out-pointers remain deferred until XBind IR
supports records and out-parameter lowering.
