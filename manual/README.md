# The Libstone manual

Libstone is a collection of compiler, intermediate-representation, virtual-machine,
assembler, and language-interoperability libraries. This manual explains the
active checkout: how its components fit together, how to build only what you
need, and how to use and extend their public interfaces.

## Contents

1. [Architecture and component guide](01-architecture.md)
2. [Building and installing](02-building.md)
3. [Configuration and dependencies](03-configuration.md)
4. [First programs and tool workflows](04-workflows.md)
5. [Embedding and API contracts](05-embedding.md)
6. [Targets, transformations, and extension points](06-extending.md)
7. [Testing and troubleshooting](07-validation.md)
8. [Reference and further reading](08-reference.md)

## Where to start

| Goal | Starting point |
| --- | --- |
| Use compiler data structures without vendor libraries | [Firestone build](02-building.md#minimal-firestone-build), then the [C++ example](05-embedding.md#a-complete-firestone-consumer) |
| Parse, transform, or execute textual IL | [Flintstone workflow](04-workflows.md#flintstone-transform-and-execute-il) |
| Discover or run optimization kernels | [Blackstone workflow](04-workflows.md#blackstone-inspect-the-kernel-catalog) and [embedding contracts](05-embedding.md#blackstone) |
| Build a compiler backend | [Architecture](01-architecture.md), [Limestone workflow](04-workflows.md#limestone-exercise-backend-stages), and [target contracts](06-extending.md#native-targets) |
| Encode instructions or assemble source | [Sandstone workflow](04-workflows.md#sandstone-encode-and-assemble) |
| Validate a binding spec or generate Lua wrappers | [XBind workflow](04-workflows.md#tourchstone-validate-and-lower-binding-specs) |
| Diagnose a failed configure, build, or test | [Troubleshooting](07-validation.md#troubleshooting) |

## Conventions and scope

- Unless stated otherwise, shell commands run from the **Libstone repository
  root**, not a component directory. `build-firestone`, `build-flintstone`, and
  similar names are separate CMake binary directories.
- Commands use a single-configuration generator and POSIX shell syntax. With a
  multi-configuration generator, add `--config Debug` to build/install commands
  and `-C Debug` to CTest; executable locations may include `Debug/`.
- `/path/to/...` denotes a path you must supply. Generated tutorial artifacts
  go under `/tmp/opencode`; create it before redirecting files there.
- The repository spells the interoperability component **Tourchstone** and the
  retired-source directory **`.decomissioned`**. Paths preserve those spellings.
- Architecture metadata, encodable instructions, native ABI support, and
  executable backend coverage are distinct capabilities. A listed target is not
  a promise of a complete production backend.
- Component manuals specify language and API details. This manual supplies the
  repository-wide entry point and links to those references rather than replacing
  their format specifications.

The source and this documentation are covered by the repository's [MIT
license](../LICENSE). Vendored dependencies have their own licenses.
