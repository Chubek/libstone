/* Compiler runtime ABI for naturally aligned atomic scalars on Linux AMD64
 * and baseline Armv8-A. The Cephyr frontend validates explicit memory orders;
 * this initial implementation strengthens all accepted orders to seq_cst.
 */
#include <stdint.h>

#define ATOMIC_WIDTH(N, T)                                                       \
    typedef T word##N __attribute__((__may_alias__));                             \
    _Static_assert(__atomic_always_lock_free(N, 0), "atomic width must be lock-free"); \
    uint64_t __cephyr_rt_atomic_load_##N(const volatile void *address) {           \
        return __atomic_load_n((const volatile word##N *)address, __ATOMIC_SEQ_CST); \
    }                                                                            \
    void __cephyr_rt_atomic_store_##N(volatile void *address, uint64_t value) {    \
        __atomic_store_n((volatile word##N *)address, (word##N)value, __ATOMIC_SEQ_CST); \
    }                                                                            \
    uint64_t __cephyr_rt_atomic_exchange_##N(volatile void *address, uint64_t value) { \
        return __atomic_exchange_n((volatile word##N *)address, (word##N)value, __ATOMIC_SEQ_CST); \
    }                                                                            \
    uint64_t __cephyr_rt_atomic_compare_exchange_##N(volatile void *address, void *expected, \
                                                       uint64_t value) {         \
        return __atomic_compare_exchange_n((volatile word##N *)address, (word##N *)expected, \
                                           (word##N)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    }                                                                            \
    ATOMIC_FETCH(N, add)                                                          \
    ATOMIC_FETCH(N, sub)                                                          \
    ATOMIC_FETCH(N, and)                                                          \
    ATOMIC_FETCH(N, or)                                                           \
    ATOMIC_FETCH(N, xor)

#define ATOMIC_FETCH(N, OP)                                                      \
    uint64_t __cephyr_rt_atomic_fetch_##OP##_##N(volatile void *address, uint64_t value) { \
        return __atomic_fetch_##OP((volatile word##N *)address, (word##N)value, __ATOMIC_SEQ_CST); \
    }

ATOMIC_WIDTH(1, uint8_t)
ATOMIC_WIDTH(2, uint16_t)
ATOMIC_WIDTH(4, uint32_t)
ATOMIC_WIDTH(8, uint64_t)

void __cephyr_rt_atomic_thread_fence(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void __cephyr_rt_atomic_signal_fence(void) {
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}
