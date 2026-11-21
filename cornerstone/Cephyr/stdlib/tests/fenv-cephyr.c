#include <fenv.h>
#include <float.h>

#if defined(__clang__) && !defined(__CEPHYR__)
#pragma STDC FENV_ACCESS ON
#endif

_Static_assert(FE_ALL_EXCEPT == (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT),
               "FE_ALL_EXCEPT comprises the advertised flags");

/* Native Cephyr callers only exercise library calls and integer control
   flow. No claim is made about the translation of FENV_ACCESS or C FP. */
static int test_library(void) {
    fenv_t initial, saved, held;
    fexcept_t flags;
    const fenv_t *defaults = FE_DFL_ENV;
    if (fegetenv(&initial) || fegetround() != FE_TONEAREST ||
        FLT_ROUNDS != 1 || fetestexcept(FE_ALL_EXCEPT))
        return 1;
    if (fesetround(FE_TOWARDZERO) || fegetround() != FE_TOWARDZERO || FLT_ROUNDS != 0)
        return 2;
    if (fesetround(FE_UPWARD) || fegetround() != FE_UPWARD || FLT_ROUNDS != 2)
        return 3;
    if (fesetround(FE_DOWNWARD) || fegetround() != FE_DOWNWARD || FLT_ROUNDS != 3)
        return 4;
    if (!fesetround(-1) || fegetround() != FE_DOWNWARD)
        return 5;
    if (feraiseexcept(FE_INVALID | FE_OVERFLOW) ||
        fetestexcept(FE_ALL_EXCEPT) != (FE_INVALID | FE_OVERFLOW) ||
        fegetexceptflag(&flags, FE_INVALID | FE_DIVBYZERO) || fegetenv(&saved))
        return 6;
    if (feclearexcept(FE_INVALID) || fetestexcept(FE_ALL_EXCEPT) != FE_OVERFLOW ||
        feraiseexcept(FE_DIVBYZERO) || fesetexceptflag(&flags, FE_INVALID | FE_DIVBYZERO) ||
        fetestexcept(FE_ALL_EXCEPT) != (FE_INVALID | FE_OVERFLOW))
        return 7;
    if (feraiseexcept(0) || feclearexcept(0) || fesetexceptflag(&flags, 0) ||
        fetestexcept(0) || fetestexcept(FE_ALL_EXCEPT) != (FE_INVALID | FE_OVERFLOW))
        return 8;
    if (feholdexcept(&held) || fegetround() != FE_DOWNWARD || fetestexcept(FE_ALL_EXCEPT))
        return 9;
    if (fesetround(FE_UPWARD) || feraiseexcept(FE_UNDERFLOW | FE_INEXACT) ||
        feupdateenv(&saved) || fegetround() != FE_DOWNWARD ||
        fetestexcept(FE_ALL_EXCEPT) != (FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT))
        return 10;
    if (fesetenv(defaults) || fegetround() != FE_TONEAREST || fetestexcept(FE_ALL_EXCEPT))
        return 11;
    if (fesetenv(&held) || fegetround() != FE_DOWNWARD ||
        fetestexcept(FE_ALL_EXCEPT) != (FE_INVALID | FE_OVERFLOW))
        return 12;
    return fesetenv(&initial) || fegetround() != FE_TONEAREST ||
           fetestexcept(FE_ALL_EXCEPT) ? 13 : 0;
}

#ifndef __CEPHYR__
/* These use explicit instructions to test the machine state independently
   of the C compiler's floating evaluation model. */
#include <signal.h>
#include <stdlib.h>

static const double half = 1.5, minus_half = -1.5, zero = 0.0;

#if defined(__x86_64__)
static unsigned int mxcsr(void) {
    unsigned int value;
    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
}

static unsigned short x87_control(void) {
    unsigned short value;
    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
}

static int test_round(int positive, int negative) {
    long x87_pos, x87_neg;
    long sse_pos, sse_neg;
    __asm__ volatile("fldl %2\n\tfistpq %0\n\tfldl %2\n\tfchs\n\tfistpq %1" :
                     "=m"(x87_pos), "=m"(x87_neg) : "m"(half) : "st");
    __asm__ volatile("cvtsd2si %2, %0\n\tcvtsd2si %3, %1" :
                     "=&r"(sse_pos), "=r"(sse_neg) : "m"(half), "m"(minus_half));
    return x87_pos == positive && x87_neg == negative &&
           sse_pos == positive && sse_neg == negative;
}

static int test_hardware_flags(void) {
    unsigned short control = x87_control() ^ FE_DOWNWARD;
    __asm__ volatile("fldcw %0" : : "m"(control));
    if (fegetround() >= 0 || FLT_ROUNDS != -1 || fesetround(FE_TONEAREST))
        return 20; /* Foreign code can desynchronize x87 and SSE modes. */

    /* Different flags in the two units, then selective clear and restore. */
    __asm__ volatile("fld1\n\tfdivl %0\n\tfstp %%st(0)" : : "m"(zero) : "st");
    if (fetestexcept(FE_ALL_EXCEPT) != FE_DIVBYZERO || (mxcsr() & FE_DIVBYZERO))
        return 21;
    __asm__ volatile("movsd %0, %%xmm0\n\tdivsd %0, %%xmm0" : : "m"(zero) : "xmm0");
    if (fetestexcept(FE_ALL_EXCEPT) != (FE_DIVBYZERO | FE_INVALID))
        return 22;

    fenv_t snapshot;
    unsigned short status;
    if (fegetenv(&snapshot) || feclearexcept(FE_DIVBYZERO) ||
        fetestexcept(FE_ALL_EXCEPT) != FE_INVALID || fesetenv(&snapshot))
        return 23;
    __asm__ volatile("fnstsw %0" : "=am"(status));
    if ((status & FE_ALL_EXCEPT) != FE_DIVBYZERO || (mxcsr() & FE_ALL_EXCEPT) != FE_INVALID)
        return 24;
    return 0;
}

static int enable_divzero_trap(int sse) {
    if (sse) {
        unsigned int control = mxcsr() & ~(FE_DIVBYZERO << 7);
        __asm__ volatile("ldmxcsr %0" : : "m"(control));
    } else {
        unsigned short control = x87_control() & ~FE_DIVBYZERO;
        __asm__ volatile("fldcw %0" : : "m"(control));
    }
    return 1;
}
#else
static int test_round(int positive, int negative) {
    double pos, neg;
    __asm__ volatile("frinti %d0, %d1" : "=w"(pos) : "w"(half));
    __asm__ volatile("frinti %d0, %d1" : "=w"(neg) : "w"(minus_half));
    return pos == positive && neg == negative;
}

static int test_hardware_flags(void) {
    double discarded;
    __asm__ volatile("fdiv %d0, %d1, %d2" : "=w"(discarded) : "w"(1.0), "w"(zero));
    return fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO ? 0 : 21;
}

static int enable_divzero_trap(int sse) {
    unsigned long control;
    (void)sse;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control));
    control |= FE_DIVBYZERO << 8;
    __asm__ volatile("msr fpcr, %0" : : "r"(control));
    __asm__ volatile("mrs %0, fpcr" : "=r"(control));
    return (control & (FE_DIVBYZERO << 8)) != 0; /* Trap enables can be RAZ/WI. */
}
#endif

static int test_hardware(void) {
    if (!test_round(2, -2) ||
        fesetround(FE_TOWARDZERO) || !test_round(1, -1) ||
        fesetround(FE_UPWARD) || !test_round(2, -1) ||
        fesetround(FE_DOWNWARD) || !test_round(1, -2))
        return 25;
    if (fesetenv(FE_DFL_ENV))
        return 26;
    return test_hardware_flags();
}

static void expected_trap(int sig) {
    _Exit(sig == SIGFPE ? 0 : 27);
}

static int test_traps(int sse, int update) {
    fexcept_t flags;
    fenv_t saved;
    if (feraiseexcept(FE_DIVBYZERO) || fegetexceptflag(&flags, FE_DIVBYZERO) ||
        feclearexcept(FE_ALL_EXCEPT) || signal(SIGFPE, SIG_DFL) == SIG_ERR)
        return 28;
    if (!enable_divzero_trap(sse))
        return 0;
    /* Setting status flags must succeed without delivering the trap. */
    if (fesetexceptflag(&flags, FE_DIVBYZERO) || fetestexcept(FE_DIVBYZERO) != FE_DIVBYZERO)
        return 29;
#if defined(__x86_64__)
    __asm__ volatile("fwait" : : : "memory");
#endif
    /* Holding the environment must disable the hardware trap. */
    if (feclearexcept(FE_DIVBYZERO) || feholdexcept(&saved) || feraiseexcept(FE_DIVBYZERO) ||
        fetestexcept(FE_DIVBYZERO) != FE_DIVBYZERO || signal(SIGFPE, expected_trap) == SIG_ERR)
        return 30;
    if (update) {
        feupdateenv(&saved);
    } else {
        fesetenv(&saved);
        feraiseexcept(FE_DIVBYZERO);
    }
    return 31; /* A supported enabled trap should have exited via SIGFPE. */
}

int main(int argc, char **argv) {
    if (argc == 2)
        return test_traps(argv[1][0] == 's', argv[1][1] == 'u');
    int status = test_library();
    return status ? status : test_hardware();
}
#else
int main(void) {
    return test_library();
}
#endif
