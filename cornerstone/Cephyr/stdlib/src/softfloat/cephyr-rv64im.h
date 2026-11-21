/* Select compiler-rt's integer-only paths even on compilers that set the
 * RISC-V ABI macro but do not define the generic __SOFTFP__ macro. */
#if !defined(__riscv) || __riscv_xlen != 64 || !defined(__riscv_mul) || \
    !defined(__riscv_div) || \
    !defined(__riscv_float_abi_soft) || !defined(__LP64__) || \
    defined(__riscv_flen) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Cephyr soft-float helpers require little-endian RV64IM with the LP64 ABI"
#endif

#ifndef __SOFTFP__
#define __SOFTFP__ 1
#endif
