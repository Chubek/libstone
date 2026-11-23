/* Shared bit-level interface for the target libc and host constant folder.
 * Widths: 32, 64, 80 (explicit integer bit), 128. Little-endian word order.
 * Results are rounded once, to nearest with ties to even. */
#ifndef CEPHYR_REAL_BITS_H
#define CEPHYR_REAL_BITS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint64_t lo, hi; } cephyr_real_bits;
cephyr_real_bits cephyr_real_calc(unsigned width, char op, cephyr_real_bits a, cephyr_real_bits b);
cephyr_real_bits cephyr_real_cast(unsigned to, unsigned from, cephyr_real_bits value);
cephyr_real_bits cephyr_real_from_int(unsigned to, int negative, uint64_t magnitude);
uint64_t cephyr_real_to_int(unsigned from, cephyr_real_bits value, unsigned bits, int is_signed);
/* -1, 0, +1 in numeric order, 2 when unordered. */
int cephyr_real_cmp(unsigned width, cephyr_real_bits a, cephyr_real_bits b);
#ifdef __cplusplus
}
#endif
#endif
