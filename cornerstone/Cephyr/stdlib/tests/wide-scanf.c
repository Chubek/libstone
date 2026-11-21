#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int scan_text(const wchar_t *text, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vswscanf(text, format, args);
    va_end(args);
    return result;
}

static int scan_file(FILE *file, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfwscanf(file, format, args);
    va_end(args);
    return result;
}

static int scan_stdin(const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vwscanf(format, args);
    va_end(args);
    return result;
}

static void numbers(void)
{
    signed char a;
    short b;
    long c, z;
    long long d;
    intmax_t j;
    ptrdiff_t t;
    int octal, hex, decimal, count;
    wchar_t next;
    assert(scan_text(L"-128 -32768 -9223372036854775808 -9223372036854775808 +42 -9 17",
                     L"%hhd %hd %ld %lld %jd %zd %td", &a, &b, &c, &d, &j, &z, &t) == 7);
    assert(a == SCHAR_MIN && b == SHRT_MIN && c == LONG_MIN && d == LLONG_MIN &&
           j == 42 && z == -9 && t == 17);
    assert(swscanf(L" \t017 -0X2a +98", L"%i %i %2d%n%lc",
                   &octal, &hex, &decimal, &count, &next) == 4);
    assert(octal == 15 && hex == -42 && decimal == 9 && count == 14 && next == L'8');
    assert(swscanf(L" \v3", L"%i%hhn%hn%ln%lln%jn%zn%tn",
                   &decimal, &a, &b, &c, &d, &j, &z, &t) == 1);
    assert(decimal == 3 && a == 3 && b == 3 && c == 3 && d == 3 &&
           j == 3 && z == 3 && t == 3);

    unsigned char ua;
    unsigned short ub;
    unsigned uc;
    unsigned long ul, ut;
    unsigned long long ull;
    uintmax_t uj;
    size_t uz;
    assert(swscanf(L"-1 177777 0Xff abc 18446744073709551615 0xAB 44 -7",
                   L"%hhu %ho %X %lx %llu %jx %zu %tu",
                   &ua, &ub, &uc, &ul, &ull, &uj, &uz, &ut) == 8);
    assert(ua == UCHAR_MAX && ub == USHRT_MAX && uc == 255 && ul == 0xabc &&
           ull == ULLONG_MAX && uj == 0xab && uz == 44 && ut == ULONG_MAX - 6);

    wchar_t pointer[32];
    void *originals[] = { &z, NULL };
    for (size_t i = 0; i < sizeof(originals) / sizeof(*originals); ++i) {
        void *restored = &t;
        assert(swprintf(pointer, sizeof(pointer) / sizeof(*pointer), L"%p", originals[i]) > 0);
        assert(swscanf(pointer, L"%p", &restored) == 1 && restored == originals[i]);
    }
}

static void text(void)
{
    char word[8], tail;
    unsigned char set[8];
    signed char fixed[] = { 'x', 'x', 'x' };
    int value, count;
    assert(swscanf(L"tag \t%99 skip ]-az=Z", L"tag%%%2d %*s %4[]a-z-]%n=%c",
                   &value, set, &count, &tail) == 3);
    assert(value == 99 && count == 18 && strcmp((char *)set, "]-az") == 0 && tail == 'Z');
    assert(swscanf(L"one]two", L"%[^]]%*c%3s", word, set) == 2 &&
           strcmp(word, "one") == 0 && strcmp((char *)set, "two") == 0);
    assert(swscanf(L"-aZ", L"%[-a]%c", word, &tail) == 2 &&
           strcmp(word, "-a") == 0 && tail == 'Z');
    assert(swscanf(L"z-A!", L"%[z-A]%c", word, &tail) == 2 &&
           strcmp(word, "z-A") == 0 && tail == '!');
    assert(swscanf(L" a", L"%[a]", word) == 0);
    assert(swscanf(L" \t", L"%2c", fixed) == 1 &&
           fixed[0] == ' ' && fixed[1] == '\t' && fixed[2] == 'x');
    assert(swscanf(L"q", L"%3c", fixed) == 0 &&
           fixed[0] == ' ' && fixed[1] == '\t' && fixed[2] == 'x');
    assert(swscanf(L"abc", L"%*2c%c", &tail) == 1 && tail == 'c');

    wchar_t wide[12], wc[] = { L'x', L'x', L'x' }, next;
    assert(swscanf(L" \tword|xy!", L"%2lc%4ls|%2l[a-z]%lc",
                   wc, wide, wide + 5, &next) == 4);
    assert(wc[0] == L' ' && wc[1] == L'\t' && wc[2] == L'x' &&
           wcscmp(wide, L"word") == 0 && wcscmp(wide + 5, L"xy") == 0 && next == L'!');
    assert(swscanf(L"\u00e9\u0402\u0403!\u0100", L"\u00e9%l[\u0401-\u0403]!%lc%n",
                   wide, &next, &count) == 2);
    assert(wcscmp(wide, L"\u0402\u0403") == 0 && next == 0x100 && count == 5);

    wchar_t input[270], copy[270];
    char narrow[270];
    wmemset(input, L'a', 269);
    input[269] = 0;
    assert(swscanf(input, L"%269lc%n", copy, &count) == 1 &&
           count == 269 && wmemcmp(input, copy, 269) == 0);
    copy[0] = L'x';
    assert(swscanf(input, L"%270lc", copy) == 0 && copy[0] == L'x');
    assert(swscanf(input, L"%ls", copy) == 1 && wcscmp(input, copy) == 0);
    assert(swscanf(input, L"%s", narrow) == 1 && strlen(narrow) == 269 && narrow[268] == 'a');
}

static void failures(void)
{
    int value = 23, n = -1;
    assert(swscanf(L"", L"%d", &value) == EOF && value == 23);
    assert(swscanf(L" \t ", L"%lc", &(wchar_t){ 0 }) == 1);
    assert(swscanf(L" \t ", L"%d", &value) == EOF && value == 23);
    assert(swscanf(L"q", L"%d%n", &value, &n) == 0 && value == 23 && n == -1);
    assert(swscanf(L"", L"y") == EOF && swscanf(L"x", L"y") == 0);
    assert(swscanf(L"", L"") == 0 && swscanf(L" \f ", L" \n ") == 0);
    assert(swscanf(L"", L"%n%d", &n, &value) == 0 && n == 0 && value == 23);
    assert(swscanf(L"123", L"%*d%d", &value) == 0 && value == 23);
    assert(swscanf(L"%", L"%%%d", &value) == EOF && value == 23);
    assert(swscanf(L"+", L"%d", &value) == 0 && value == 23);
    assert(swscanf(L"0x", L"%i", &value) == 0 && value == 23);

    char bytes[] = "XXX";
    errno = 0;
    assert(swscanf(L"ok\u00e9!", L"%s", bytes) == EOF &&
           errno == EILSEQ && strcmp(bytes, "XXX") == 0);
    errno = 0;
    assert(swscanf(L"1 \u00e9", L"%d %*c", &value) == 1 && errno == EILSEQ && value == 1);
    errno = 0;
    assert(swscanf(L"1", L"%18446744073709551616d", &value) == EOF &&
           errno == EOVERFLOW && value == 1);
    errno = 0;
    assert(swscanf(L"2147483648", L"%d", &value) == EOF && errno == ERANGE && value == 1);
    float f;
    memset(&f, 0, sizeof(f));
    errno = 0;
    assert(swscanf(L"1", L"%f", &f) == EOF && errno == ENOSYS);
    for (size_t i = 0; i < sizeof(f); ++i)
        assert(((unsigned char *)&f)[i] == 0);
}

static void streams(void)
{
    int value = 0, n = -1;
    FILE *file = tmpfile();
    assert(file != NULL && fwide(file, 0) == 0);
    assert(fwscanf(file, L"") == 0 && fwide(file, 0) > 0 && !feof(file));
    assert(fputws(L"1: +x 0x. 078! 0x9;", file) >= 0);
    assert(fputwc(L'\0', file) == 0 && fputwc(L'Z', file) == L'Z');
    rewind(file);

    assert(fwscanf(file, L"%d%n!", &value, &n) == 1 && value == 1 && n == 1);
    assert(getwc(file) == L':');
    assert(fwscanf(file, L"%d", &value) == 0 && value == 1 && getwc(file) == L'x');
    assert(fwscanf(file, L"%i", &value) == 0 && value == 1 && getwc(file) == L'.');
    assert(fwscanf(file, L"%i", &value) == 1 && value == 7 && getwc(file) == L'8');
    assert(getwc(file) == L'!');
    assert(fwscanf(file, L"%2i", &value) == 0 && value == 7 && getwc(file) == L'9');
    assert(getwc(file) == L';');
    assert(ungetwc(L'\u0402', file) == 0x402);
    assert(fwscanf(file, L"x") == 0 && !feof(file) && getwc(file) == 0x402);
    assert(ungetwc(L'4', file) == L'4');
    assert(scan_file(file, L"%d%n", &value, &n) == 1 && value == 4 && n == 1);
    wchar_t wide[] = { L'x', L'x', L'x' };
    assert(fwscanf(file, L"%2lc", wide) == 1 &&
           wide[0] == 0 && wide[1] == L'Z' && wide[2] == L'x' && !feof(file));
    assert(fwscanf(file, L"%lc", wide) == EOF && feof(file) && !ferror(file));
    assert(ungetwc(L'\u0402', file) == 0x402 && !feof(file));
    char narrow = 'x';
    errno = 0;
    assert(fwscanf(file, L"%c", &narrow) == EOF && errno == EILSEQ && narrow == 'x');
    rewind(file);
    errno = 0;
    assert(fwscanf(file, L"%*Lg") == EOF && errno == ENOSYS && getwc(file) == L'1');
    assert(fclose(file) == 0);

    file = tmpfile();
    assert(file != NULL && fputs("1", file) == 0);
    errno = 0;
    assert(fwscanf(file, L"") == EOF && ferror(file) && errno == EINVAL && fwide(file, 0) < 0);
    assert(fclose(file) == 0);

    file = fopen("/dev/null", "w");
    assert(file != NULL && fwide(file, 0) == 0);
    errno = 0;
    assert(fwscanf(file, L"%d", &value) == EOF && ferror(file) &&
           !feof(file) && errno == EBADF && fwide(file, 0) > 0);
    assert(fclose(file) == 0);

    char name[L_tmpnam];
    assert(tmpnam(name) == name);
    file = fopen(name, "wx");
    assert(file != NULL && fputs("\x80" "a", file) >= 0);
    assert(freopen(name, "r", file) == file && fwide(file, 0) == 0);
    errno = 0;
    assert(fwscanf(file, L"%lc", wide) == EOF && ferror(file) && !feof(file) &&
           errno == EILSEQ && wide[0] == 0);
    clearerr(file);
    assert(getwc(file) == L'a' && fclose(file) == 0 && remove(name) == 0);
}

int main(void)
{
    numbers();
    text();
    failures();
    streams();

    /* Run with stdin piped from: printf '18? 24\n' */
    int value;
    wchar_t ch;
    assert(fwide(stdin, 0) == 0);
    assert(wscanf(L"%d!", &value) == 1 && value == 18 && fwide(stdin, 0) > 0);
    assert(scan_stdin(L"%lc %d", &ch, &value) == 2 && ch == L'?' && value == 24);
    assert(getwchar() == L'\n' && wscanf(L"%d", &value) == EOF && feof(stdin));
    return 0;
}
