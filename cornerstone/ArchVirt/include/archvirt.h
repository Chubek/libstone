#ifndef ARCHVIRT_H
#define ARCHVIRT_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArchVirtConfig {
    size_t struct_size;
    const char *machine, *backend, *archml_path;
    const char *kernel_path, *image_path, *firmware_path;
    const char *cmdline;
    uint64_t memory_mb, instruction_limit;
    uint32_t flags; /* ARCHVIRT_DOMIX | ARCHVIRT_DOMIOS */
} ArchVirtConfig;
enum { ARCHVIRT_DOMIX = 1, ARCHVIRT_DOMIOS = 2 };
typedef uint8_t (*ArchVirtSerial)(void* user, int is_write, uint8_t value);

void* archvirt_create(void);
int archvirt_initialize(void* vm, const ArchVirtConfig* config);
/* Set before initialization; NULL selects the terminal console. */
int archvirt_set_serial(void* vm, ArchVirtSerial serial, void* user);
int archvirt_start(void* vm);
int archvirt_wait(void* vm);
void archvirt_stop(void* vm);
void archvirt_destroy(void* vm);
const char* archvirt_last_error(void* vm);
const char* archvirt_version(void);

/* Source/ABI-compatible Domvirt entry points use the same machine runtime. */
void* domvirt_create(void);
int domvirt_initialize(void* vm, const char* kernel, const char* initrd, const char* cmdline, uint64_t mem_mb);
int domvirt_run(void* vm);
void domvirt_stop(void* vm);
void domvirt_destroy(void* vm);
const char* domvirt_version(void);
#ifdef __cplusplus
}
#endif
#endif
