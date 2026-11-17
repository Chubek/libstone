/* SandAS source extension SDK, version 1. Callbacks run once per expansion,
 * before layout. Generated source is recursively processed. Registrations
 * belong to a context and survive assembly calls; user data remains borrowed.
 * A callback returns 0 on success and emits via the supplied sink. It must not
 * recursively assemble on the same context or throw across this C boundary. */
#ifndef SANDSTONE_ASM_SDK_H
#define SANDSTONE_ASM_SDK_H
#include "assembler.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SANDSTONE_ASM_SDK_VERSION 1
typedef int (*sandstone_source_sink)(void *sink, const char *text, size_t size);
typedef int (*sandstone_source_handler)(void *user, const char *arguments,
    sandstone_source_sink emit, void *sink, char *error, size_t error_size);
/* Names beginning with '.' are directives; other names are macro commands.
 * Built-in names and duplicate registrations are rejected. */
int sandstone_asm_register_directive(sandstone_asm_t *, const char *name,
    sandstone_source_handler, void *user);
int sandstone_asm_unregister_directive(sandstone_asm_t *, const char *name);
int sandstone_asm_define_macro(sandstone_asm_t *, const char *name,
    const char *parameters, const char *body);
/* ArchML .insn NAME[,VALUE...] uses the profile's checked encoder. Operands
 * must be assembly-time constants, in the profile's declared order. */
int sandstone_asm_load_archml(sandstone_asm_t *, const char *path);
int sandstone_asm_new_archml(sandstone_asm_t **, const char *path,
    char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
