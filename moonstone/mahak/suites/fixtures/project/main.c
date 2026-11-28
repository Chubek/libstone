#include "shared.h"
int main(void)
{
    const unsigned long total = unit_a() + unit_b() + unit_c() + unit_d();
    return total == EXPECTED_SUM ? 0 : 1;
}
