#ifndef _CEPHYR_FLOAT_H
#define _CEPHYR_FLOAT_H

/* ISO C11 5.2.4.2.2: format characteristics of the bundled Linux LP64 ABIs.
   The frontend may not be able to generate code for every described type. */
#define FLT_RADIX 2
#if defined(__linux__) && defined(__LP64__) && \
    (defined(__x86_64__) || defined(__AARCH64EL__))
int __cephyr_flt_rounds(void);
#define FLT_ROUNDS (__cephyr_flt_rounds())
#elif defined(__linux__) && defined(__LP64__) && defined(__riscv) && \
      __riscv_xlen == 64 && defined(__riscv_float_abi_soft) && !defined(__riscv_flen) && \
      defined(__riscv_mul) && defined(__riscv_div)
#define FLT_ROUNDS 1 /* The RV64IM soft-float helpers always round to nearest. */
#else
#define FLT_ROUNDS (-1) /* No bundled rounding-mode query for this target. */
#endif
#define FLT_EVAL_METHOD 0

#define FLT_MANT_DIG 24
#define FLT_DIG 6
#define FLT_DECIMAL_DIG 9
#define FLT_MIN_EXP (-125)
#define FLT_MAX_EXP 128
#define FLT_MIN_10_EXP (-37)
#define FLT_MAX_10_EXP 38
#define FLT_MAX 0x1.fffffep+127F
#define FLT_EPSILON 0x1p-23F
#define FLT_MIN 0x1p-126F
#define FLT_TRUE_MIN 0x1p-149F
#define FLT_HAS_SUBNORM 1

#define DBL_MANT_DIG 53
#define DBL_DIG 15
#define DBL_DECIMAL_DIG 17
#define DBL_MIN_EXP (-1021)
#define DBL_MAX_EXP 1024
#define DBL_MIN_10_EXP (-307)
#define DBL_MAX_10_EXP 308
#define DBL_MAX 0x1.fffffffffffffp+1023
#define DBL_EPSILON 0x1p-52
#define DBL_MIN 0x1p-1022
#define DBL_TRUE_MIN 0x1p-1074
#define DBL_HAS_SUBNORM 1

#if defined(__x86_64__)
#define LDBL_MANT_DIG 64
#define LDBL_DIG 18
#define LDBL_DECIMAL_DIG 21
#define LDBL_MAX 0x1.fffffffffffffffep+16383L
#define LDBL_EPSILON 0x1p-63L
#define LDBL_TRUE_MIN 0x1p-16445L
#elif defined(__aarch64__) || (defined(__riscv) && __riscv_xlen == 64)
#define LDBL_MANT_DIG 113
#define LDBL_DIG 33
#define LDBL_DECIMAL_DIG 36
#define LDBL_MAX 0x1.ffffffffffffffffffffffffffffp+16383L
#define LDBL_EPSILON 0x1p-112L
#define LDBL_TRUE_MIN 0x1p-16494L
#else
#error "float.h requires a supported Cephyr libc target"
#endif

#define LDBL_MIN_EXP (-16381)
#define LDBL_MAX_EXP 16384
#define LDBL_MIN_10_EXP (-4931)
#define LDBL_MAX_10_EXP 4932
#define LDBL_MIN 0x1p-16382L
#define LDBL_HAS_SUBNORM 1
#define DECIMAL_DIG LDBL_DECIMAL_DIG

#endif
