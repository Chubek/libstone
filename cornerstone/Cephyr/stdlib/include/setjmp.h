#ifndef _CEPHYR_SETJMP_H
#define _CEPHYR_SETJMP_H

#if defined(__x86_64__)
typedef unsigned long jmp_buf[8];
#elif defined(__aarch64__)
typedef unsigned long jmp_buf[21];
#elif defined(__riscv) && __riscv_xlen == 64
typedef unsigned long jmp_buf[14];
#else
#error "setjmp.h requires a supported Cephyr libc target"
#endif

#ifdef __CEPHYR__
/* An ordinary native MIR call is not sufficient for setjmp. */
#define __CEPHYR_NONLOCAL_JUMPS_UNAVAILABLE 1
#else
int __cephyr_setjmp(jmp_buf) __attribute__((returns_twice));
#define setjmp(env) __cephyr_setjmp(env)
_Noreturn void longjmp(jmp_buf, int);
#endif

#endif
