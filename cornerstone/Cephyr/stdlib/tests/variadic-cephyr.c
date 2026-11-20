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
    return puts(text) == EOF;
}
