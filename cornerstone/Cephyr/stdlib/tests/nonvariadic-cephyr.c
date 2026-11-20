#define __CEPHYR__ 1
#include <stdio.h>

#if !defined(__CEPHYR_HAS_VARIADIC_CALLS) && \
    !defined(__CEPHYR_VARIADIC_IO_UNAVAILABLE)
#error "Cephyr must not expose variadic I/O before ABI support is enabled"
#endif

int main(void)
{
    return puts("non-variadic Cephyr stdio is available") == EOF;
}
