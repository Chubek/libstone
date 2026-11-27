# Public dependency catalog

The `catalog/*.tsv` files are auditable dependency inventories, not a promise
that every tracked header builds on every platform. The columns are:

| Column | Meaning |
| --- | --- |
| `family`, `name` | Stable catalog identity; for Firestone, the header path relative to `firestone/`. |
| `source_path` | Git-tracked path relative to the Libstone checkout, or `-`. |
| `include` | Include spelling emitted for a checkout consumer, or `-`. |
| `cmake_target` | Public CMake target, or `-`. A target is not a raw linker flag. |
| `status` | `available` when the documented consumer probe passed, `unverified` when there is public API/build evidence but no such tested consumer, or `unavailable` when the described integration cannot be used. |
| `evidence` | Location or description of the public API, build rules, or verification. |
| `bundle`, `license` | Whether Amalgam8 can inline the header and the reviewed redistribution notice, if eligible. |

Firestone coverage is the intersection of `git ls-files` and the public CMake
installation: the two aggregate headers, `LibDSL.hpp`, and `.hpp` files in
installed component directories. Tests, examples, binding definitions, and
untracked files are not part of the inventory. The audit checks the component
list, export rules, and install rules rather than treating any `lib*` directory
as public. Most Firestone headers are **unverified**;
`BigInt.hpp`, `BigFloat.hpp`, `Complex128.hpp`, and `LibDSL.hpp` have installed
consumer probes. Other headers may need platform or SDK configuration not
supplied by Amalgam8.

From the repository root:

```sh
python3 cornerstone/Amalgam8/tools/catalog_audit.py
python3 cornerstone/Amalgam8/tools/catalog_audit.py --tsv
python3 cornerstone/Amalgam8/tools/catalog_audit.py --family firestone --probe-firestone
```

If Firestone intentionally changes its public header inventory, review the
CMake evidence and use `--family firestone --refresh-firestone` to regenerate
the TSV and the compiler's C++ index. Re-run the installed probes on the new
inventory. Native family records are maintained separately and checked against
the same schema and tracked source paths.

## Firestone use declarations

`(use firestone:BigInt)` and `(use firestone libbignum/BigInt.hpp)` both resolve
to `firestone:libbignum/BigInt.hpp`. A full path is preferred. An extensionless
path or unique basename is also accepted; ambiguous basenames report candidate
paths. Unknown or non-public headers are rejected. Known but unverified public
headers still emit an external include and are reported as unverified.

External Firestone includes retain the historical `firestone/...` spelling.
Add the repository root (checkout) or `<prefix>/include` (installed package)
to the consumer's include search path. When using CMake, link the recorded
`Firestone::...` target so its transitive compile/link usage requirements apply.
Native Firestone builds can also use their target's native header spellings
(for example, `<libbignum/BigInt.hpp>` and `<LibDSL.hpp>`); the audit's installed
probes use those spellings without the repository on their include path.

## Bundling

Bundling is opt-in and currently limited to the reviewed `BigInt`, `BigFloat`,
and `Complex128` header snapshots. Other dependencies remain external. Only the
approved repository paths are read. An include-root override is a **source tree
root** containing `LICENSE` and `firestone/libbignum/`, not the C++ compiler's
include flag. If no root is given, packaging uses the current working directory.
Neither external resolution nor generated source depends on the override's
filesystem location. Use only trusted source trees.

The generated header includes the byte-for-byte Firestone MIT notice. The
packager replaces each snapshot's leading `#pragma once` with a stable,
header-specific macro guard shared across generated files; it replaces only
the reviewed local `BigFloat.hpp` → `BigInt.hpp` include with its guarded
contents in place. The original order, standard-library includes, and other
header bytes are retained. Missing or changed snapshots or license bytes fail,
rather than silently redistributing unreviewed contents. Do not also include
the corresponding original Firestone headers in the same translation unit:
their filesystem-based `#pragma once` does not recognize an inlined copy.

Run the packaging regression test with:

```sh
python3 cornerstone/Amalgam8/tests/W6_standalone.py
```
