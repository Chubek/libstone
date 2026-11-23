#ifndef _CEPHYR_FENV_H
#define _CEPHYR_FENV_H

/* C11 §7.6 library interface. This doesn't enable frontend FENV_ACCESS. */
#if !defined(__linux__) || !defined(__LP64__)
#error "Cephyr libc fenv.h requires Linux LP64"
#elif defined(__x86_64__)
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
#elif defined(__AARCH64EL__)
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
#elif defined(__riscv) && __riscv_xlen == 64 && defined(__riscv_float_abi_soft) && \
      !defined(__riscv_flen) && defined(__riscv_mul) && defined(__riscv_div)
/* RV64IM integer arithmetic: fixed nearest-even, no status flags. */
typedef unsigned int fenv_t;
#define FE_ALL_EXCEPT 0
#define FE_TONEAREST 0
#else
#error "Cephyr libc fenv.h requires AMD64, little-endian AArch64 or soft-float RV64IM"
#endif

typedef unsigned int fexcept_t;

#ifndef FE_ALL_EXCEPT
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)
#endif

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
