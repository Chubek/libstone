#ifndef CEPHYR_MIR_VERIFY_H
#define CEPHYR_MIR_VERIFY_H

#include <Backend.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Check bounded Archon MIR v1 text and the native LP64 IR invariants without
 * selecting instructions, allocating registers, or emitting code. */
ArchonBackendStatus cephyr_verify_mir(const char *text, size_t size,
                                      ArchonBackendDiagnostic *diagnostic);
/* Calls the visitor only on verified input. Spans are [start, end), in
 * function/block/instruction order. All pointers expire at return. */
typedef void (*CephyrMIRVisitor)(void *, const ArchonMIRModule *, const size_t *, const size_t *);
ArchonBackendStatus cephyr_visit_mir(const char *, size_t, CephyrMIRVisitor, void *,
                                    ArchonBackendDiagnostic *);

#ifdef __cplusplus
}
#endif

#endif
