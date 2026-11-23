#ifndef CEPHYR_PLUGIN_H
#define CEPHYR_PLUGIN_H

#include "CephyrSDK.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CEPHYR_PLUGIN_ENTRY_V1 "cephyr_plugin_v1"
#define CEPHYR_PLUGIN_MAX_FILES 8u

typedef const cephyr_sdk *(*cephyr_plugin_entry_v1)(void);

/* Export this entry point from a trusted host shared object. Return a
 * non-null, immutable, borrowed SDK descriptor. The descriptor, all
 * registration storage, and callback user state must remain valid until
 * the compile call ends. Entry and callbacks must not throw across this
 * C ABI. Concurrent compile calls may use the same loaded object. */
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const cephyr_sdk *cephyr_plugin_v1(void);

#ifdef __cplusplus
}
#endif

#endif
