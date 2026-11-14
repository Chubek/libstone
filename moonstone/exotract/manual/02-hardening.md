# Exotract implementation and verification plan

Baseline: LibStone `91293264a531603dfd2319e64b3c1aa40a609783`.
Exotract was an untracked user addition; preserve its public C++ API and unsafe
metadata format. Vendor revisions are the trees recorded at that commit.

The work strengthens one declaration pipeline, with ExoML remaining the type
and semantic contract authority. Global substitutions are frozen and validated
before execution. CDecl ingestion commits a complete graph only after structural
validation; every failure clears prior declarations. Compiler syntax success
does not mask extraction failures. Output publication is atomic and happens only
after success (diagnostic manifests remain available on standard output).

Native subprocesses use separate argv, bounded output and time, private process
groups, and closed input. Temporary outputs are exclusively created in the
destination directory. Cache reuse requires fresh compiler preprocessing,
syntax checking and matching freshly extracted ExoML provenance; imported
artifacts cannot become trusted cache entries. Unknown semantics remain unknown.

Tests cover real optional vendors, linked and CLI ExoML discovery, C11 macro
profiles, invalid imports and stale state, subprocess descendants and budgets,
concurrent publication, independent LMDB writers and corrupt entries, Lua error
paths, installation consumers, and a pinned real MuLIB implementation.
On1x source emission and safe adapters require a supported consuming grammar and
reviewed matching contracts; neither may be fabricated from raw pointers.
