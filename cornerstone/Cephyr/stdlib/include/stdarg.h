#ifndef _CEPHYR_STDARG_H
#define _CEPHYR_STDARG_H

#if defined(__CEPHYR__)
#error "Cephyr does not support va_list or variadic function definitions"
#else
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dst, src) __builtin_va_copy(dst, src)
#endif

#endif
