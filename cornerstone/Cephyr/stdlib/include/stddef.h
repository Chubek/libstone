#ifndef _CEPHYR_STDDEF_H
#define _CEPHYR_STDDEF_H

/* Types for the packaged Linux LP64 targets. */
typedef unsigned long size_t;
typedef long ptrdiff_t;
#ifdef __aarch64__
typedef unsigned int wchar_t;
#else
typedef int wchar_t;
#endif

/* The bundled LP64 targets support 16-byte fundamental alignment. */
typedef struct {
    _Alignas(16) unsigned char __cephyr_aligned;
} max_align_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
