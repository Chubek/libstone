#ifndef _CEPHYR_STDATOMIC_H
#define _CEPHYR_STDATOMIC_H

#if !defined(__CEPHYR__) || defined(__STDC_NO_ATOMICS__)
#error "<stdatomic.h> requires Cephyr's bundled Linux AMD64 or AArch64 backend"
#else

#include <stddef.h>
#include <stdint.h>

typedef enum memory_order {
    memory_order_relaxed,
    memory_order_consume,
    memory_order_acquire,
    memory_order_release,
    memory_order_acq_rel,
    memory_order_seq_cst
} memory_order;

#define ATOMIC_BOOL_LOCK_FREE 2
#define ATOMIC_CHAR_LOCK_FREE 2
#define ATOMIC_CHAR16_T_LOCK_FREE 2
#define ATOMIC_CHAR32_T_LOCK_FREE 2
#define ATOMIC_WCHAR_T_LOCK_FREE 2
#define ATOMIC_SHORT_LOCK_FREE 2
#define ATOMIC_INT_LOCK_FREE 2
#define ATOMIC_LONG_LOCK_FREE 2
#define ATOMIC_LLONG_LOCK_FREE 2
#define ATOMIC_POINTER_LOCK_FREE 2

typedef _Atomic(_Bool) atomic_bool;
typedef _Atomic(char) atomic_char;
typedef _Atomic(signed char) atomic_schar;
typedef _Atomic(unsigned char) atomic_uchar;
typedef _Atomic(short) atomic_short;
typedef _Atomic(unsigned short) atomic_ushort;
typedef _Atomic(int) atomic_int;
typedef _Atomic(unsigned int) atomic_uint;
typedef _Atomic(long) atomic_long;
typedef _Atomic(unsigned long) atomic_ulong;
typedef _Atomic(long long) atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(uint_least16_t) atomic_char16_t;
typedef _Atomic(uint_least32_t) atomic_char32_t;
typedef _Atomic(wchar_t) atomic_wchar_t;
typedef _Atomic(int_least8_t) atomic_int_least8_t;
typedef _Atomic(uint_least8_t) atomic_uint_least8_t;
typedef _Atomic(int_least16_t) atomic_int_least16_t;
typedef _Atomic(uint_least16_t) atomic_uint_least16_t;
typedef _Atomic(int_least32_t) atomic_int_least32_t;
typedef _Atomic(uint_least32_t) atomic_uint_least32_t;
typedef _Atomic(int_least64_t) atomic_int_least64_t;
typedef _Atomic(uint_least64_t) atomic_uint_least64_t;
typedef _Atomic(int_fast8_t) atomic_int_fast8_t;
typedef _Atomic(uint_fast8_t) atomic_uint_fast8_t;
typedef _Atomic(int_fast16_t) atomic_int_fast16_t;
typedef _Atomic(uint_fast16_t) atomic_uint_fast16_t;
typedef _Atomic(int_fast32_t) atomic_int_fast32_t;
typedef _Atomic(uint_fast32_t) atomic_uint_fast32_t;
typedef _Atomic(int_fast64_t) atomic_int_fast64_t;
typedef _Atomic(uint_fast64_t) atomic_uint_fast64_t;
typedef _Atomic(intptr_t) atomic_intptr_t;
typedef _Atomic(uintptr_t) atomic_uintptr_t;
typedef _Atomic(size_t) atomic_size_t;
typedef _Atomic(ptrdiff_t) atomic_ptrdiff_t;
typedef _Atomic(intmax_t) atomic_intmax_t;
typedef _Atomic(uintmax_t) atomic_uintmax_t;

typedef struct { unsigned char __cephyr_state; } atomic_flag;

#define ATOMIC_VAR_INIT(value) (value)
#define ATOMIC_FLAG_INIT { 0 }
#define kill_dependency(value) (value)

#define atomic_init(object, value) __cephyr_atomic_init((object), (value))
#define atomic_is_lock_free(object) __cephyr_atomic_is_lock_free((object))
#define atomic_load_explicit(object, order) __cephyr_atomic_load((object), (order))
#define atomic_store_explicit(object, value, order) __cephyr_atomic_store((object), (value), (order))
#define atomic_exchange_explicit(object, value, order) \
    __cephyr_atomic_exchange((object), (value), (order))
#define atomic_compare_exchange_strong_explicit(object, expected, desired, success, failure) \
    __cephyr_atomic_compare_exchange((object), (expected), (desired), (success), (failure))
#define atomic_compare_exchange_weak_explicit(object, expected, desired, success, failure) \
    atomic_compare_exchange_strong_explicit((object), (expected), (desired), (success), (failure))
#define atomic_fetch_add_explicit(object, operand, order) \
    __cephyr_atomic_fetch_add((object), (operand), (order))
#define atomic_fetch_sub_explicit(object, operand, order) \
    __cephyr_atomic_fetch_sub((object), (operand), (order))
#define atomic_fetch_and_explicit(object, operand, order) \
    __cephyr_atomic_fetch_and((object), (operand), (order))
#define atomic_fetch_or_explicit(object, operand, order) \
    __cephyr_atomic_fetch_or((object), (operand), (order))
#define atomic_fetch_xor_explicit(object, operand, order) \
    __cephyr_atomic_fetch_xor((object), (operand), (order))

#define atomic_load(object) atomic_load_explicit((object), memory_order_seq_cst)
#define atomic_store(object, value) atomic_store_explicit((object), (value), memory_order_seq_cst)
#define atomic_exchange(object, value) atomic_exchange_explicit((object), (value), memory_order_seq_cst)
#define atomic_compare_exchange_strong(object, expected, desired) \
    atomic_compare_exchange_strong_explicit((object), (expected), (desired), memory_order_seq_cst, memory_order_seq_cst)
#define atomic_compare_exchange_weak(object, expected, desired) \
    atomic_compare_exchange_weak_explicit((object), (expected), (desired), memory_order_seq_cst, memory_order_seq_cst)
#define atomic_fetch_add(object, operand) atomic_fetch_add_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_sub(object, operand) atomic_fetch_sub_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_and(object, operand) atomic_fetch_and_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_or(object, operand) atomic_fetch_or_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_xor(object, operand) atomic_fetch_xor_explicit((object), (operand), memory_order_seq_cst)

#define atomic_flag_test_and_set_explicit(object, order) \
    __cephyr_atomic_flag_test_and_set(&((object)->__cephyr_state), (order))
#define atomic_flag_clear_explicit(object, order) \
    __cephyr_atomic_flag_clear(&((object)->__cephyr_state), (order))
#define atomic_flag_test_and_set(object) atomic_flag_test_and_set_explicit((object), memory_order_seq_cst)
#define atomic_flag_clear(object) atomic_flag_clear_explicit((object), memory_order_seq_cst)
#define atomic_thread_fence(order) __cephyr_atomic_thread_fence((order))
#define atomic_signal_fence(order) __cephyr_atomic_signal_fence((order))

#endif
#endif
