#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <wchar.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

#if !defined(__CEPHYR__) || defined(__CEPHYR_HAS_AMD64_STDARG)
static int list_buffer(wchar_t *out, size_t size, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vswprintf(out, size, format, args);
    va_end(args);
    return result;
}

static int list_stream(FILE *stream, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfwprintf(stream, format, args);
    va_end(args);
    return result;
}

static int list_stdout(const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vwprintf(format, args);
    va_end(args);
    return result;
}
#endif

int main(void)
{
    wchar_t buffer[80];
    wchar_t *out = buffer + 1;
    buffer[0] = L'!';
    buffer[6] = L'!';
    CHECK(swprintf(out, 5, L"abcd") == 4 && wcscmp(out, L"abcd") == 0);
    errno = 0;
    CHECK(swprintf(out, 5, L"abcde") < 0 && errno == EOVERFLOW);
    CHECK(buffer[0] == L'!' && buffer[5] == 0 && buffer[6] == L'!');
    CHECK(swprintf(out, 0, L"") < 0 && *out == L'a');
    CHECK(swprintf(out, 1, L"") == 0 && *out == 0);
    CHECK(swprintf(out, 5, L"a%*d", INT_MAX, 1) < 0 && errno == EOVERFLOW);
    CHECK(wcscmp(out, L"a") == 0);
    CHECK(swprintf(out, 5, L"%*d", INT_MIN, 1) < 0 && errno == EOVERFLOW && *out == 0);
    CHECK(swprintf(out, 5, L"%2147483648d", 1) < 0 && errno == EOVERFLOW);
    CHECK(swprintf(out, 5, L"%.2147483648d", 1) < 0 && errno == EOVERFLOW);
    CHECK(swprintf(out, 5, L"%") < 0 && errno == EINVAL);

    int count = -1;
    wchar_t one[1];
    one[0] = L'W';
    char bad[2];
    bad[0] = 'b';
    bad[1] = (char)0x80;
    CHECK(swprintf(buffer, 80, L"[%3lc][%*.*s][%.1ls]%%:%n",
                   (wint_t)L'Q', -4, 1, bad, one, &count) == 16);
    CHECK(count == 16 && wcscmp(buffer, L"[  Q][b   ][W]%:") == 0);
    CHECK(swprintf(buffer, 80, L"%.0s%.0ls", bad + 1, one) == 0);
    CHECK(swprintf(buffer, 80, L"%.*ls", -1, L"hi") == 2);
    CHECK(swprintf(buffer, 80, L"%c%lc!", 0, (wint_t)0) == 3);
    CHECK(buffer[0] == 0 && buffer[1] == 0 && buffer[2] == L'!' && buffer[3] == 0);
    CHECK(swprintf(buffer, 80, L"%.2s", bad) < 0 && errno == EILSEQ);
    CHECK(swprintf(buffer, 80, L"%c", 0x80) < 0 && errno == EILSEQ);
    CHECK(swprintf(buffer, 80, L"%lc", (wint_t)0x80) == 1 && buffer[0] == 0x80);

    CHECK(swprintf(buffer, 80, L"%+06d|%#08x|%#.0o|%.0d|%05.3d|%-5u",
                   42, 18U, 0U, 0, 2, 9U) == 30);
    CHECK(wcscmp(buffer, L"+00042|0x000012|0||  002|9    ") == 0);
    CHECK(swprintf(buffer, 80, L"%hhd %hu %lld %jd %zu %td %#X",
                   (signed char)-7, (unsigned short)65535, LLONG_MIN,
                   (intmax_t)-9, (size_t)12345678901UL, (ptrdiff_t)-8, 42U) == 52);
    CHECK(wcscmp(buffer, L"-7 65535 -9223372036854775808 -9 12345678901 -8 0X2A") == 0);
    signed char hh = -1;
    short h = -1;
    long l = -1;
    long long ll = -1;
    intmax_t j = -1;
    long z = -1;
    ptrdiff_t t = -1;
    CHECK(swprintf(buffer, 80, L"%p%hhn%hn%ln%lln%jn%zn%tn", (void *)0,
                   &hh, &h, &l, &ll, &j, &z, &t) == 3);
    CHECK(wcscmp(buffer, L"0x0") == 0);
    CHECK(hh == 3 && h == 3 && l == 3 && ll == 3 && j == 3 && z == 3 && t == 3);

    static wchar_t large[4096];
    CHECK(swprintf(large, 4096, L"%.4095u", 9U) == 4095);
    for (int i = 0; i < 4094; ++i)
        CHECK(large[i] == L'0');
    CHECK(large[4094] == L'9' && large[4095] == 0);

#ifndef __CEPHYR__
    CHECK(swprintf(buffer, 80, L"before%f", 1.0) < 0 && errno == ENOSYS);
    CHECK(wcscmp(buffer, L"before") == 0);
    CHECK(swprintf(buffer, 80, L"%La", 1.0L) < 0 && errno == ENOSYS);
#endif

    FILE *file = tmpfile();
    CHECK(file != NULL && fwide(file, 0) == 0);
    CHECK(fwprintf(file, L"") == 0 && fwide(file, 0) > 0);
    CHECK(fwprintf(file, L"%3s%lc%n", "go", (wint_t)0, &count) == 4 && count == 4);
    CHECK(fwprintf(file, L"%lc", (wint_t)0x80) < 0 && errno == EILSEQ && ferror(file));
    rewind(file);
    CHECK(fgetwc(file) == L' ' && fgetwc(file) == L'g' &&
          fgetwc(file) == L'o' && fgetwc(file) == 0 && fgetwc(file) == WEOF);
    CHECK(fclose(file) == 0);

    file = tmpfile();
    CHECK(file != NULL && fputc('x', file) == 'x' && fwide(file, 0) < 0);
    CHECK(fwprintf(file, L"") < 0 && errno == EINVAL && fwide(file, 0) < 0);
    CHECK(fclose(file) == 0);

    file = fopen("/dev/null", "r");
    CHECK(file != NULL && fwprintf(file, L"bad") < 0);
    CHECK(errno == EBADF && ferror(file) && fwide(file, 0) > 0);
    CHECK(fclose(file) == 0);

    CHECK(wprintf(L"wide:%d", -7) == 7 && fwide(stdout, 0) > 0);
#if !defined(__CEPHYR__) || defined(__CEPHYR_HAS_AMD64_STDARG)
    CHECK(list_buffer(buffer, 80, L"%d:%ls", 3, L"list") == 6);
    CHECK(wcscmp(buffer, L"3:list") == 0);
    file = tmpfile();
    CHECK(file != NULL && list_stream(file, L"%ls", L"v") == 1 && fwide(file, 0) > 0);
    rewind(file);
    CHECK(fgetwc(file) == L'v' && fclose(file) == 0);
    CHECK(list_stdout(L"!\n") == 2);
#else
    CHECK(wprintf(L"!\n") == 2);
#endif
    return 0;
}
