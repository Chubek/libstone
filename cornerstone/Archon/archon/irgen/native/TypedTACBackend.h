/* Archon typed TAC to runtime native backend adapter. */
#ifndef ARCHON_TYPED_TAC_BACKEND_H
#define ARCHON_TYPED_TAC_BACKEND_H

#include "Backend.h"
#include "IR.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile one function to native assembly. ABI parameters must appear first in
 * block zero, the entry, and are passed in declaration order. Phis must appear
 * first in non-entry blocks and read inputs simultaneously on each edge.
 * The native verifier checks reachability, edges to entry, and dominance.
 *
 * The ABI carries 64-bit bits/uint/sint and permits a void return. All returns
 * must agree in TAC type. A function with no return is void. Internal bool
 * values are canonical 0/1 constants, comparisons, copies, or phi results.
 * Word arithmetic wraps; word bitwise ops use all 64 bits. Equality compares
 * bits, and less-than requires signed or unsigned word operands. Effects,
 * calls, addresses, narrow words, and shifts are not translated.
 *
 * Adapter caps: 256 blocks, 4096 values, 64 ABI parameters, 16384 inputs,
 * and 64 KiB of strings, with an additional verification work budget.
 * Program tables, symbol, and options are borrowed immutable valid storage
 * for the call. Artifact must be empty and is published only on success;
 * release it with archon_backend_artifact_destroy(). The diagnostic is optional.
 * Adapter verification diagnostics use global TAC operation indices; backend
 * diagnostics are forwarded unchanged (MIR coordinates).
 */
ArchonBackendStatus archon_typed_tac_backend_compile(
    const IR_Program *program, const char *symbol,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);
/* Same input profile; flags are the compile flags defined by Backend.h. */
ArchonBackendStatus archon_typed_tac_backend_compile_ex(
    const IR_Program *program, const char *symbol,
    const ArchonBackendOptions *options, unsigned flags, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);

#ifdef __cplusplus
}
#endif
#endif
