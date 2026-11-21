#ifndef _CEPHYR_STDARG_H
#define _CEPHYR_STDARG_H

#if defined(__CEPHYR__)
#if defined(__CEPHYR_HAS_AMD64_STDARG)
typedef struct __cephyr_amd64_va_record {
    unsigned int __gp_offset;
    unsigned int __fp_offset;
    void *__overflow_arg_area;
    void *__reg_save_area;
} va_list[1];
#define va_start(ap, last) __cephyr_va_start((ap), last)
#define va_arg(ap, type) __cephyr_va_arg((ap), (type *)0)
#define va_end(ap) ((void)(ap))
#define va_copy(dst, src) ((void)(*(dst) = *(src)))
#elif defined(__CEPHYR_HAS_AARCH64_STDARG)
/* AAPCS64 uses a struct, not AMD64's one-element array. Forward lists by
 * pointer in Cephyr: AAPCS64 aggregate arguments are not supported yet. */
typedef struct __cephyr_aarch64_va_record {
    void *__stack;
    void *__gr_top;
    void *__vr_top;
    int __gr_offs;
    int __vr_offs;
} va_list;
#define va_start(ap, last) __cephyr_va_start(&(ap), last)
#define va_arg(ap, type) __cephyr_va_arg(&(ap), (type *)0)
#define va_end(ap) ((void)(&(ap)))
#define va_copy(dst, src) ((void)((dst) = (src)))
#elif defined(__CEPHYR_HAS_RV64_STDARG)
/* RV64IM LP64 uses one pointer into the contiguous argument save area. */
typedef void *va_list;
#define va_start(ap, last) __cephyr_va_start((ap), last)
#define va_arg(ap, type) __cephyr_va_arg((ap), (type *)0)
#define va_end(ap) __cephyr_va_end((ap))
#define va_copy(dst, src) ((void)((dst) = (src)))
#else
#error "Cephyr <stdarg.h> requires a bundled SysV AMD64, AAPCS64 or RV64IM LP64 backend"
#endif
#else
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dst, src) __builtin_va_copy(dst, src)
#endif

#endif
