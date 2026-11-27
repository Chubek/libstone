# LibDSL integration record

The twelve workstreams were W01 (broker and integration), W02 (shared
services), W03 (interactive syntax), W04 (Terminfo), W05 (escape/style), W06
(options), W07 (editor/session), W08 (live analysis), W09 (directives/scripts),
W10 (specification and macros), W11 (attributes and scoping), and W12
(execution and optimization). The implementation workers staged disjoint
fragments and reported their results to W01. The fragments are merged into
`../LibDSL.hpp`; tests live in `../../tests/libdsl`.

The Linux-only [development broker](broker.py) used an owner-only System V
message queue with separate routes for each W01↔worker pair. Every assignment
was acknowledged on its original correlation ID. The blotter was a
`/dev/shm`-backed shared map; updates were serialized with a file lock and
appended with section revisions. [Contracts and decisions](contracts.md)
describe the interface freeze; `blotter.json` is the final snapshot. The
broker is not a build tool or a LibDSL dependency.

See [the manual](../../docs/LibDSL.md) for the public interfaces, trust
boundaries, portability, and implementation limits.
