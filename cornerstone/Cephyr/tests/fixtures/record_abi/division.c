#include <stdlib.h>
#include <inttypes.h>

#if !defined(__CEPHYR_HAS_NATIVE_DIVISION_API) || defined(__CEPHYR_STRUCT_VALUE_API_UNAVAILABLE)
#error expected the ABI-native division declarations on bundled RV64IM LP64
#endif

int main(void)
{
    div_t i = div(-101, 9);
    ldiv_t l = ldiv(0x100000007l, -8l);
    lldiv_t ll = lldiv(-0x10000000003ll, 16ll);
    imaxdiv_t m = imaxdiv(-0x800000000000000l, -31l);
    if (i.quot != -11 || i.rem != -2)
        return 41;
    if (l.quot != -0x20000000l || l.rem != 7)
        return 42;
    if (ll.quot != -0x1000000000ll || ll.rem != -3)
        return 43;
    if (m.quot != 0x800000000000000l / 31 || m.rem != -(0x800000000000000l % 31))
        return 44;
    return 0;
}
