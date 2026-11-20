#include <ctype.h>
#include <errno.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int ch = getchar();
    (void)argv;
    if (ch == EOF)
        return 1;
    if (putchar(toupper(ch)) == EOF)
        return 2;
    if (putchar('\n') == EOF)
        return 3;
    /* A failed seek on a pipe must not discard read-ahead. */
    if (argc > 1 &&
        (fseek(stdin, 0, SEEK_CUR) == 0 || errno != ESPIPE || getchar() != '!'))
        return 4;
    if (getchar() != EOF || !feof(stdin) || ferror(stdin))
        return 5;
    clearerr(stdin);
    return feof(stdin) ? 6 : 0;
}
