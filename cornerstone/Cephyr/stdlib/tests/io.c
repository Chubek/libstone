#include <ctype.h>
#include <stdio.h>

int main(void)
{
    int ch = getchar();
    if (ch == EOF)
        return 1;
    if (putchar(toupper(ch)) == EOF)
        return 2;
    if (putchar('\n') == EOF)
        return 3;
    if (getchar() != EOF || !feof(stdin) || ferror(stdin))
        return 4;
    clearerr(stdin);
    return feof(stdin) ? 5 : 0;
}
