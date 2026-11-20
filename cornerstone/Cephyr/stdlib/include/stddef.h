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

#if defined(__CEPHYR__)
/*
 * The current parser cannot represent long double or an explicitly aligned
 * aggregate.  Do not lie about an 8-byte substitute on a 16-byte-aligned ABI.
 */
#define __CEPHYR_MAX_ALIGN_T_UNAVAILABLE 1
#else
typedef struct {
    long long __cephyr_ll;
    long double __cephyr_ld;
} max_align_t;
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
