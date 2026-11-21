#include <stdio.h>
#include <string.h>

#ifndef __CEPHYR_HAS_VARIADIC_CALLS
#error "This test requires direct scalar variadic calls"
#endif

int main(void)
{
    char text[80];
    signed char small = -7;
    unsigned short positive = 65535;
    const char *word = "stack";
    int length = snprintf(text, sizeof(text), "%d %hu %ld %d %d %d %d %s %lu",
                          small, positive, -9000000000L,
                          4, 5, 6, 7, word, 12345678901234UL);
    const char *expected = "-7 65535 -9000000000 4 5 6 7 stack 12345678901234";

    if (length != (int)strlen(expected) || strcmp(text, expected) != 0)
        return 1;
    int values[7];
    char end;
    if (sscanf("1 2 3 4 5 6 7!", "%d %d %d %d %d %d %d%c",
               &values[0], &values[1], &values[2], &values[3],
               &values[4], &values[5], &values[6], &end) != 8 || end != '!')
        return 2;
    for (int i = 0; i < 7; ++i) {
        if (values[i] != i + 1)
            return 3;
    }
    return puts(text) == EOF;
}
