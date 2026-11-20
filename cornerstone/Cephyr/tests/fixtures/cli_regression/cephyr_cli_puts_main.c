#include <stdio.h>

unsigned long cephyr_cli_accumulate(const unsigned *values, unsigned count);

static const unsigned values[5] = {2, 3, 5, 7, 11};

int main(void) {
    if (cephyr_cli_accumulate(values, 5) != 28)
        return 20;
    if (puts("cephyr multi-tu ok") < 0)
        return 21;
    return 0;
}
