#undef assert

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
_Noreturn void __cephyr_assert_fail(const char *, const char *, int,
                                     const char *);
#define assert(expression)                                                     \
    ((expression) ? (void)0                                                    \
                  : __cephyr_assert_fail(#expression, __FILE__, __LINE__,       \
                                         __func__))
#endif
