#undef assert

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#if defined(__CEPHYR__)
_Noreturn void __cephyr_assert_fail_at(const char *, const char *, int);
/* cparser-iso11 does not represent the predefined identifier __func__. */
#define assert(expression)                                                     \
    ((expression) ? (void)0                                                    \
                  : __cephyr_assert_fail_at(#expression, __FILE__, __LINE__))
#else
_Noreturn void __cephyr_assert_fail(const char *, const char *, int,
                                    const char *);
#define assert(expression)                                                     \
    ((expression) ? (void)0                                                    \
                  : __cephyr_assert_fail(#expression, __FILE__, __LINE__,       \
                                         __func__))
#endif
#endif
