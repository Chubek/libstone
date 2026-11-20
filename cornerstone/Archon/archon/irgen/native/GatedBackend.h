/* Archon generated Gated SSA to native backend adapter. */
#ifndef ARCHON_GATED_BACKEND_H
#define ARCHON_GATED_BACKEND_H

#include "Backend.h"
#include "IR.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile one straight-line scalar function from the matching generated IR
 * library. The caller supplies an exported symbol and the nonzero Gated value
 * ID returned at the end of the single block; it need not be the last node.
 * All parameter nodes must use distinct indices forming [0, parameter count).
 * Parameters and results use the native scalar i64 ABI.
 *
 * The entire program must verify and fit the supported subset: 64-bit bits,
 * uint and sint are treated as word bit patterns, with constants and reviewed
 * pure register-value copy, wrapping add/subtract/multiply, and bitwise
 * and/or/xor only. No implicit control edges, effects, machine registers or
 * architectural state are inferred.
 *
 * Input is borrowed and immutable during the call. The artifact must be
 * empty on entry; on failure it is unchanged. Release a successful result
 * with archon_backend_artifact_destroy. A diagnostic may be NULL; for a
 * node-specific adapter error, instruction is the zero-based Gated node
 * index. Errors from the backend retain native MIR diagnostic coordinates.
 */
ArchonBackendStatus archon_gated_backend_compile(
    const struct archon_ir_program *program, const char *symbol, uint32_t result_id,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);
/* Same input profile; flags are the compile flags defined by Backend.h. */
ArchonBackendStatus archon_gated_backend_compile_ex(
    const struct archon_ir_program *program, const char *symbol, uint32_t result_id,
    const ArchonBackendOptions *options, unsigned flags, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);

#ifdef __cplusplus
}
#endif
#endif
