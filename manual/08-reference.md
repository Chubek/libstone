# 8. Reference and further reading

## Source map

| Path | Purpose |
| --- | --- |
| [`CMakeLists.txt`](../CMakeLists.txt) | Aggregate component selection |
| [`cmake/LibstoneBuild.cmake`](../cmake/LibstoneBuild.cmake) | Shared defaults, warnings, dependency-file checks |
| [`cmake/MLVMDependencies.cmake`](../cmake/MLVMDependencies.cmake) | Dependency paths and legacy cache aliases |
| [`scripts/get-deps.sh`](../scripts/get-deps.sh) | Explicit dependency acquisition; not part of CMake configuration |
| `third_party/` | Default vendored dependency root |
| `.decomissioned/` | Retired source trees; not an implicit fallback |

## Tool locations

These are typical single-config **standalone** build paths. Installed tools use
their executable names under the installation `bin` directory.

| Tool | Standalone path | Root aggregate path |
| --- | --- | --- |
| Flintstone IL tool | `build-flintstone/flintstone` | `build/flintstone/flintstone` |
| Blackstone catalog | `build-blackstone/blackstone-kernels` | `build/blackstone/blackstone-kernels` |
| Limestone CLI | `build-limestone/limestone-cli` | `build/limestone/limestone-cli` |
| SymAS compiler | `build-sandstone/compiler/sandstone-spec-compiler` | `build/sandstone/compiler/sandstone-spec-compiler` |
| Sandstone assembler | `build-sandstone/assembler/sandstone-as` | `build/sandstone/assembler/sandstone-as` |
| XBind | `build-xbind/xbind` | `build/tourchstone/xbind/xbind` |

The CMake target `flintstone-cli` produces the executable `flintstone`;
`xbind_cli` produces `xbind`. Do not confuse build target names with output names.
Optional Tourchstone native tools are not available in the default retired-qBFD
configuration.

## Formats and representations

| Input or artifact | Meaning | Reference |
| --- | --- | --- |
| Flintstone `.flint` | Textual multi-level, typed IL | [Language](../flintstone/manual/03-language.md) |
| FSVM bytecode | Portable Firestone VM module, not a native object | [VM guide](../firestone/docs/VM.md) |
| Blackstone kernel `.lua` | Executable optimization declaration and implementation | [Kernel guide](../blackstone/README.md#kernel-format) |
| Limestone Infobank `.isa` | Source-located target metadata | [Metacode](../limestone/manual/04-metacode-and-the-infobank.md) |
| Limestone textual ILs | Selection, scheduling, allocation, tracing, and machine representations | [CLI/languages](../limestone/manual/20-cli-and-textual-languages.md) |
| MachineIR exchange | Versioned C++/D interchange, separate from native code | [MachineIR](../limestone/manual/11-machineir-and-cpp-d-exchange.md) |
| Sandstone `.dsymspec` | Lua architecture-spec source | [Spec pipeline](../sandstone/compiler/stdspec/README.md) |
| Sandstone `.symas` | Architecture assembly databank, magic `0xCEEB09` | [Normative format](../sandstone/SYMAS.md) |
| Sandstone small `.isa` | Lightweight instruction forms used by small-ISA loaders/SandSLAB | [ISA guide](../sandstone/isa/README.md) |
| Sandstone `obj` output | JSON sections/symbols/relocations; not ELF | [Object guide](../sandstone/docs/object.md) |
| XBind `.xbind` | Executable Lua manifest describing selected C bindings | [XBind manual](../tourchstone/xbind/MANUAL.md) |

Limestone and Sandstone both use `.isa` filenames, but their formats and loaders
are not interchangeable merely because the extensions match.

## Component documentation

- **Firestone:** [overview](../firestone/README.md), [API guide](../firestone/docs/API.md),
  [installation](../firestone/INSTALL.md), [VM contracts](../firestone/docs/VM.md),
  [foundation libraries](../firestone/docs/Foundations.md).
- **Blackstone:** [runtime and embedding](../blackstone/README.md),
  [kernel inventory](../blackstone/docs/kernels.md).
- **Flintstone:** [manual](../flintstone/manual/README.md), including
  [standard library/imports](../flintstone/manual/10-stdlib-and-imports.md) and
  [diagnostics/limits](../flintstone/manual/09-testing.md).
- **Limestone:** [manual](../limestone/manual/README.md),
  [implementation boundaries](../limestone/IMPLEMENTATION.md),
  [pipeline API](../limestone/limestone/README.md).
- **Sandstone:** [overview](../sandstone/README.md), [task guide](../sandstone/GUIDE.md),
  [installation](../sandstone/INSTALL.md), [SymAS](../sandstone/SYMAS.md),
  [SandSLAB](../sandstone/sandslab/README.md).
- **Tourchstone:** [overview](../tourchstone/README.md),
  [XBind manual](../tourchstone/xbind/MANUAL.md),
  [validation regimen](../tourchstone/REGIMEN.md).

When contributing, consult the applicable component `AGENTS.md`, preserve public
ownership/diagnostic contracts, and edit generator inputs rather than generated
outputs. Component specifications and executable tests are the detailed sources
of truth behind this repository-level guide.
