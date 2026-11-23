/* ISO C11 §7.6. Use hardware state where present, with no library shadow. */
#include <fenv.h>
#include <float.h>

#if FE_ALL_EXCEPT
/* Evaluate only for enabled hardware traps. In non-stop mode the flags can
   be set exactly without an extra inexact from overflow or underflow. */
static const struct {
    unsigned int flag;
    double numerator, denominator;
} trapping_divisions[] = {
    {FE_INVALID,   0.0,     0.0},
    {FE_DIVBYZERO, 1.0,     0.0},
    {FE_OVERFLOW,  DBL_MAX, 0.5},
    {FE_UNDERFLOW, DBL_MIN, 3.0},
    {FE_INEXACT,   2.0,     3.0},
};

#if defined(__x86_64__)
#include <stddef.h>

#define X87_FLAGS 0x3fu         /* Includes the nonstandard denormal flag. */
#define X87_STACK_FAULT 0x40u
#define X87_PENDING 0x8080u     /* B and ES bits in the status word. */
#define MXCSR_MASK_SHIFT 7
#define MXCSR_ROUND_SHIFT 3     /* Relative to the x87 rounding field. */

const fenv_t __cephyr_fe_dfl_env = {0x037f, 0, 0x1f80};

/* FNSTENV's 28-byte image is not our public fenv_t. FLDENV can change the
   x87 stack's tags and TOP, so reinstate those from the live image, not
   from an earlier point in the caller's execution. */
typedef struct {
    unsigned short control, reserved, status;
    unsigned short rest[11];
} x87_env_image;

_Static_assert(sizeof(x87_env_image) == 28 && offsetof(x87_env_image, status) == 4,
               "x87 protected-mode environment image");

static unsigned short x87_control(void) {
    unsigned short control;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    return control;
}

static unsigned short x87_status(void) {
    unsigned short status;
    __asm__ volatile("fnstsw %0" : "=am"(status));
    return status;
}

static unsigned int read_mxcsr(void) {
    unsigned int mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    return mxcsr;
}

static void write_mxcsr(unsigned int mxcsr) {
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr) : "memory");
}

static void x87_modify(unsigned int control_mask, unsigned int control,
                       unsigned int status_mask, unsigned int status) {
    x87_env_image live;
    /* FNSTENV masks x87 traps until the matching FLDENV. Neither the
       snapshot nor the restore takes a pending trap here. */
    __asm__ volatile("fnstenv %0" : "=m"(live) : : "memory");
    live.control = (live.control & ~control_mask) | control;
    live.status = ((live.status & ~status_mask) | status) & ~X87_PENDING;
    if (live.status & ~live.control & X87_FLAGS)
        live.status |= X87_PENDING;
    __asm__ volatile("fldenv %0" : : "m"(live) : "memory");
}

static void write_flags(unsigned int mask, unsigned int flags) {
    /* C exposes the union of x87 and SSE flags. Set new bits using
       MXCSR, where setting status bits can't schedule a pending trap.
       Clear selected bits in both units. SF is auxiliary to x87 invalid. */
    unsigned int clear = mask & ~flags;
    if (clear)
        x87_modify(0, 0, clear | ((clear & FE_INVALID) ? X87_STACK_FAULT : 0), 0);
    write_mxcsr((read_mxcsr() & ~mask) | flags);
}

int fetestexcept(int excepts) {
    return (x87_status() | read_mxcsr()) & excepts & FE_ALL_EXCEPT;
}

int feraiseexcept(int excepts) {
    unsigned int flags = excepts & FE_ALL_EXCEPT;
    if (!flags)
        return 0;

    unsigned int mxcsr = read_mxcsr();
    unsigned int sse_traps = flags & ~(mxcsr >> MXCSR_MASK_SHIFT);
    for (unsigned int i = 0; i < sizeof trapping_divisions / sizeof *trapping_divisions; ++i) {
        if (sse_traps & trapping_divisions[i].flag) {
            __asm__ volatile("movsd %0, %%xmm0\n\tdivsd %1, %%xmm0" : :
                             "m"(trapping_divisions[i].numerator),
                             "m"(trapping_divisions[i].denominator) : "xmm0");
        }
    }

    unsigned int x87_traps = flags & ~x87_control();
    if (x87_traps) {
        x87_modify(0, 0, x87_traps, x87_traps);
        __asm__ volatile("fwait" : : : "memory");
    }
    write_mxcsr(read_mxcsr() | flags);
    return 0;
}

int fegetround(void) {
    unsigned int round = x87_control() & FE_TOWARDZERO;
    if (round != ((read_mxcsr() >> MXCSR_ROUND_SHIFT) & FE_TOWARDZERO))
        return -1;
    return round;
}

int fesetround(int round) {
    if ((unsigned int)round & ~FE_TOWARDZERO)
        return 1;
    x87_modify(FE_TOWARDZERO, round, 0, 0);
    write_mxcsr((read_mxcsr() & ~(FE_TOWARDZERO << MXCSR_ROUND_SHIFT)) |
                (round << MXCSR_ROUND_SHIFT));
    return 0;
}

int fegetenv(fenv_t *envp) {
    envp->__x87_control = x87_control();
    envp->__x87_status = x87_status() & (X87_FLAGS | X87_STACK_FAULT);
    envp->__mxcsr = read_mxcsr();
    return 0;
}

int fesetenv(const fenv_t *envp) {
    x87_modify(0xffffu, envp->__x87_control, X87_FLAGS | X87_STACK_FAULT, envp->__x87_status);
    write_mxcsr(envp->__mxcsr);
    return 0;
}

int feholdexcept(fenv_t *envp) {
    fegetenv(envp);
    x87_modify(X87_FLAGS, X87_FLAGS, X87_FLAGS | X87_STACK_FAULT, 0);
    write_mxcsr((envp->__mxcsr & ~X87_FLAGS) | (X87_FLAGS << MXCSR_MASK_SHIFT));
    return 0;
}

#else /* AArch64 */

#define FPSR_EXCEPTIONS (FE_ALL_EXCEPT | 0x80ul)  /* Includes input denormal. */
#define FPCR_TRAP_SHIFT 8
#define FPCR_TRAPS (FPSR_EXCEPTIONS << FPCR_TRAP_SHIFT)

const fenv_t __cephyr_fe_dfl_env = {0, 0};

static unsigned long read_fpcr(void) {
    unsigned long fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    return fpcr;
}

static unsigned long read_fpsr(void) {
    unsigned long fpsr;
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    return fpsr;
}

static void write_fpcr(unsigned long fpcr) {
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr) : "memory");
}

static void write_fpsr(unsigned long fpsr) {
    __asm__ volatile("msr fpsr, %0" : : "r"(fpsr) : "memory");
}

static void write_flags(unsigned int mask, unsigned int flags) {
    write_fpsr((read_fpsr() & ~(unsigned long)mask) | flags);
}

int fetestexcept(int excepts) {
    return read_fpsr() & excepts & FE_ALL_EXCEPT;
}

int feraiseexcept(int excepts) {
    unsigned int flags = excepts & FE_ALL_EXCEPT;
    if (!flags)
        return 0;
    unsigned int traps = flags & (read_fpcr() >> FPCR_TRAP_SHIFT);
    for (unsigned int i = 0; i < sizeof trapping_divisions / sizeof *trapping_divisions; ++i) {
        if (traps & trapping_divisions[i].flag) {
            double discarded;
            __asm__ volatile("fdiv %d0, %d1, %d2" : "=w"(discarded) :
                             "w"(trapping_divisions[i].numerator),
                             "w"(trapping_divisions[i].denominator));
        }
    }
    write_fpsr(read_fpsr() | flags);
    return 0;
}

int fegetround(void) {
    return read_fpcr() & FE_TOWARDZERO;
}

int fesetround(int round) {
    if ((unsigned int)round & ~FE_TOWARDZERO)
        return 1;
    write_fpcr((read_fpcr() & ~(unsigned long)FE_TOWARDZERO) | round);
    return 0;
}

int fegetenv(fenv_t *envp) {
    envp->__fpcr = read_fpcr();
    envp->__fpsr = read_fpsr();
    return 0;
}

int fesetenv(const fenv_t *envp) {
    write_fpcr(envp->__fpcr);
    write_fpsr(envp->__fpsr);
    return 0;
}

int feholdexcept(fenv_t *envp) {
    fegetenv(envp);
    write_fpcr(envp->__fpcr & ~FPCR_TRAPS);
    write_fpsr(envp->__fpsr & ~FPSR_EXCEPTIONS);
    return (read_fpcr() & FPCR_TRAPS) != 0;
}
#endif
#else /* RV64IM: one immutable environment, already in non-stop mode. */
const fenv_t __cephyr_fe_dfl_env = 0;

static void write_flags(unsigned int mask, unsigned int flags) {
    (void)mask;
    (void)flags;
}

int fetestexcept(int excepts) { (void)excepts; return 0; }
int feraiseexcept(int excepts) { (void)excepts; return 0; }
int fegetround(void) { return FE_TONEAREST; }
int fesetround(int round) { return round != FE_TONEAREST; }
int fegetenv(fenv_t *envp) { *envp = __cephyr_fe_dfl_env; return 0; }
int feholdexcept(fenv_t *envp) { return fegetenv(envp); }
int fesetenv(const fenv_t *envp) { (void)envp; return 0; }
#endif

int feclearexcept(int excepts) {
    unsigned int mask = excepts & FE_ALL_EXCEPT;
    if (mask)
        write_flags(mask, 0);
    return 0;
}

int fegetexceptflag(fexcept_t *flagp, int excepts) {
    *flagp = fetestexcept(excepts);
    return 0;
}

int fesetexceptflag(const fexcept_t *flagp, int excepts) {
    unsigned int mask = excepts & FE_ALL_EXCEPT;
    if (mask)
        write_flags(mask, *flagp & mask);
    return 0;
}

int feupdateenv(const fenv_t *envp) {
    int raised = fetestexcept(FE_ALL_EXCEPT);
    if (fesetenv(envp))
        return 1;
    return feraiseexcept(raised);
}

#if FE_ALL_EXCEPT
int __cephyr_flt_rounds(void) {
    switch (fegetround()) {
    case FE_TOWARDZERO: return 0;
    case FE_TONEAREST:  return 1;
    case FE_UPWARD:     return 2;
    case FE_DOWNWARD:   return 3;
    default:           return -1;
    }
}
#endif
