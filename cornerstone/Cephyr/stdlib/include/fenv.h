#ifndef _CEPHYR_FENV_H
#define _CEPHYR_FENV_H

/* C11 §7.6 library interface. This doesn't enable frontend FENV_ACCESS. */
#if !defined(__linux__) || !defined(__LP64__) || \
    (!defined(__x86_64__) && !defined(__AARCH64EL__))
#error "Cephyr libc fenv.h is supported only on Linux LP64 AMD64 and little-endian AArch64"
#else

#if defined(__x86_64__)
typedef struct {
    unsigned short __x87_control;
    unsigned short __x87_status;
    unsigned int __mxcsr;
} fenv_t;

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20

#define FE_TONEAREST  0x000
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00
#else
typedef struct {
    unsigned long __fpcr;
    unsigned long __fpsr;
} fenv_t;

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x02
#define FE_OVERFLOW   0x04
#define FE_UNDERFLOW  0x08
#define FE_INEXACT    0x10

#define FE_TONEAREST  0x000000
#define FE_UPWARD     0x400000
#define FE_DOWNWARD   0x800000
#define FE_TOWARDZERO 0xc00000
#endif

typedef unsigned int fexcept_t;

#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

extern const fenv_t __cephyr_fe_dfl_env;
#define FE_DFL_ENV (&__cephyr_fe_dfl_env)

int feclearexcept(int);
int fegetexceptflag(fexcept_t *, int);
int feraiseexcept(int);
int fesetexceptflag(const fexcept_t *, int);
int fetestexcept(int);
int fegetround(void);
int fesetround(int);
int fegetenv(fenv_t *);
int feholdexcept(fenv_t *);
int fesetenv(const fenv_t *);
int feupdateenv(const fenv_t *);

#endif
#endif
