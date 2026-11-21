#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#if defined(__CEPHYR__) && !defined(__CEPHYR_HAS_VARIADIC_CALLS)
#error "This test requires direct scalar variadic calls"
#endif

static int wide_format(void)
{
    /* Use execution-set codes so the fixture needs no wide literal support. */
    wchar_t wide[4];
    wchar_t bad[3];
    char out[16];
    int count = -1;
    wide[0] = 's';
    wide[1] = 'u';
    wide[2] = 'n';
    wide[3] = 0;
    bad[0] = 'X';
    bad[1] = 0x80;
    bad[2] = 0;

    if (snprintf(out, sizeof(out), "[%3lc][%*.*ls]%n",
                 (wint_t)'Q', -5, 2, wide, &count) != 12 ||
        strcmp(out, "[  Q][su   ]") != 0 || count != 12)
        return 4;

    /* C11 treats a null %lc like an empty %ls. Ordinary %c emits a byte. */
    if (snprintf(out, sizeof(out), "[%2lc]%c!", (wint_t)0, 0) != 6 ||
        memcmp(out, "[  ]\0!", 7) != 0)
        return 5;

    /* No null terminator is needed when the precision exhausts the field. */
    wchar_t single[1];
    single[0] = 'T';
    if (snprintf(out, sizeof(out), "%.1ls%.0ls", single, bad + 1) != 1 ||
        strcmp(out, "T") != 0)
        return 6;

    if (snprintf(out, 2, "%.1ls%lc", bad, (wint_t)'Y') != 2 ||
        strcmp(out, "X") != 0 || snprintf(NULL, 0, "%5ls", wide) != 5)
        return 7;

    errno = 0;
    if (snprintf(out, sizeof(out), "prefix%ls", bad) >= 0 ||
        errno != EILSEQ || strcmp(out, "prefix") != 0)
        return 8;
    errno = 0;
    if (snprintf(NULL, 0, "%lc", (wint_t)0x80) >= 0 || errno != EILSEQ)
        return 9;

    FILE *file = tmpfile();
    if (file == NULL)
        return 10;
    if (fprintf(file, "%5ls:%lc", wide, (wint_t)'!') != 7 ||
        fwide(file, 0) >= 0)
        return 11;
    rewind(file);
    if (fread(out, 1, 7, file) != 7 || memcmp(out, "  sun:!", 7) != 0 ||
        fclose(file) != 0)
        return 12;

    /* §7.21.6.1 requires a conversion to support at least 4095 bytes. */
    wchar_t long_text[4096];
    for (int i = 0; i < 4095; ++i)
        long_text[i] = i % 2 == 0 ? 'a' : 'b';
    long_text[4095] = 0;
    char full[4096];
    if (snprintf(full, sizeof(full), "%ls", long_text) != 4095 || full[4095] != 0)
        return 13;
    for (int i = 0; i < 4095; ++i) {
        if (full[i] != long_text[i])
            return 14;
    }
    return 0;
}

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
    int wide_status = wide_format();
    if (wide_status != 0)
        return wide_status;
    return puts(text) == EOF;
}
