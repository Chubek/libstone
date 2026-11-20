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

#ifdef __cplusplus
}
#endif

#endif
