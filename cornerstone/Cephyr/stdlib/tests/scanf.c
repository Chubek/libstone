#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int scan_text(const char *text, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int count = vsscanf(text, fmt, args);
    va_end(args);
    return count;
}

static int scan_file(FILE *stream, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int count = vfscanf(stream, fmt, args);
    va_end(args);
    return count;
}

static int scan_stdin(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int count = vscanf(fmt, args);
    va_end(args);
    return count;
}

static void integers(void)
{
    signed char a;
    short b;
    int c;
    long d, z;
    ptrdiff_t t;
    intmax_t j;
    long long ll;
    assert(scan_text("-128 -32768 -2147483648 -9223372036854775808 "
                     "-9223372036854775808 +17 -17 47", "%hhd %hd %d %ld %lld %jd %zd %td",
                     &a, &b, &c, &d, &ll, &j, &z, &t) == 8);
    assert(a == SCHAR_MIN && b == SHRT_MIN && c == INT_MIN && d == LONG_MIN &&
           ll == LLONG_MIN && j == 17 && z == -17 && t == 47);

    unsigned char ua;
    unsigned short ub;
    unsigned uc;
    unsigned long ud, ut;
    unsigned long long ull;
    uintmax_t uj;
    size_t uz;
    assert(sscanf("-1 177777 0xffffffff FFFFFFFFFFFFFFFF 18446744073709551615 "
                  "0xAB 44 -7", "%hhu %ho %x %lX %llu %jx %zu %tu",
                  &ua, &ub, &uc, &ud, &ull, &uj, &uz, &ut) == 8);
    assert(ua == UCHAR_MAX && ub == USHRT_MAX && uc == UINT_MAX &&
           ud == ULONG_MAX && ull == ULLONG_MAX && uj == 0xab && uz == 44 &&
           ut == ULONG_MAX - 6);

    int octal, hex, decimal, n1, n2;
    char next;
    assert(sscanf(" 017 0X20 -24 078", "%i %i %i %n%2i%n%c",
                  &octal, &hex, &decimal, &n1, &c, &n2, &next) == 5);
    assert(octal == 15 && hex == 32 && decimal == -24 && c == 7 &&
           n1 == 14 && n2 == 16 && next == '8');
    assert(sscanf(" -0Xf +17", "%i %2d%c", &c, &decimal, &next) == 3);
    assert(c == -15 && decimal == 1 && next == '7');
    assert(sscanf(" 1", "%d%hhn%hn%ln%lln%jn%zn%tn",
                  &c, &a, &b, &d, &ll, &j, &z, &t) == 1);
    assert(a == 2 && b == 2 && d == 2 && ll == 2 && j == 2 && z == 2 && t == 2);

    char pointer[32];
    void *original = &t, *restored = NULL;
    assert(snprintf(pointer, sizeof(pointer), "%p", original) > 0);
    assert(sscanf(pointer, "%p", &restored) == 1 && restored == original);
    original = NULL;
    assert(snprintf(pointer, sizeof(pointer), "%p", original) > 0);
    assert(sscanf(pointer, "%p", &restored) == 1 && restored == original);
}

static void text_fields(void)
{
    char word[10], next;
    unsigned char letters[10];
    signed char fixed[3] = { 'x', 'x', 'x' };
    int count, value;

    assert(sscanf("tag\t %99 skip ]-az=Z", "tag%%%2d %*s %4[]a-z-]%n=%c",
                  &value, letters, &count, &next) == 3);
    assert(value == 99 && count == 18 && strcmp((char *)letters, "]-az") == 0 &&
           next == 'Z');
    assert(sscanf("abc-def", "%[^-]%c%3s", word, &next, letters) == 3 &&
           strcmp(word, "abc") == 0 && next == '-' &&
           strcmp((char *)letters, "def") == 0);
    assert(sscanf("abc]", "%[^]]%c", word, &next) == 2 &&
           strcmp(word, "abc") == 0 && next == ']');
    assert(sscanf("-q", "%[-]%c", word, &next) == 2 &&
           strcmp(word, "-") == 0 && next == 'q');
    assert(sscanf("z-A!", "%[z-A]%c", word, &next) == 2 &&
           strcmp(word, "z-A") == 0 && next == '!');
    assert(sscanf(" aa", "%[a]", word) == 0);
    assert(sscanf(" \t", "%2c", fixed) == 1 && fixed[0] == ' ' &&
           fixed[1] == '\t' && fixed[2] == 'x');
    assert(sscanf("x", "%3c", fixed) == 0 && fixed[0] == ' ' &&
           fixed[1] == '\t' && fixed[2] == 'x');
    assert(sscanf("abc", "%*2c%c", &next) == 1 && next == 'c');
    assert(sscanf("\xff!", "%1s%c", letters, &next) == 2 &&
           letters[0] == 0xff && letters[1] == 0 && next == '!');

    wchar_t wide[8] = { 0 };
    wchar_t wc[3] = { 0, 0, 'x' };
    assert(sscanf(" \twx|aAA!", "%2lc%2ls|%3l[a-zA-Z]%c", wc, wide,
                  wide + 3, &next) == 4 && wc[0] == ' ' && wc[1] == '\t' &&
           wc[2] == 'x' && wide[0] == 'w' && wide[1] == 'x');
    assert(wide[2] == 0 && wide[3] == 'a' && wide[4] == 'A' &&
           wide[5] == 'A' && wide[6] == 0 && next == '!');

    errno = 0;
    assert(sscanf("ok\x80!", "%ls", wide) == EOF &&
           errno == EILSEQ && wide[0] == 'w');
    errno = 0;
    assert(sscanf("1 \x80", "%d %*lc", &value) == 1 &&
           errno == EILSEQ && value == 1);

    char large[270], copy[270];
    wchar_t wide_copy[270];
    memset(large, 'a', sizeof(large) - 1);
    large[sizeof(large) - 1] = 0;
    assert(sscanf(large, "%269c%n", copy, &count) == 1 &&
           count == 269 && memcmp(large, copy, 269) == 0);
    memset(copy, 'x', sizeof(copy));
    assert(sscanf(large, "%270c", copy) == 0 && copy[0] == 'x');
    assert(sscanf(large, "%ls", wide_copy) == 1 && wide_copy[269] == 0);
    for (size_t i = 0; i < 269; ++i)
        assert(wide_copy[i] == 'a');
    signed char tiny = 23;
    errno = 0;
    assert(sscanf(large, "%*129c%hhn", &tiny) == EOF &&
           errno == ERANGE && tiny == 23);
}

static int filled_with(const void *object, size_t size, unsigned char byte)
{
    const unsigned char *contents = object;
    for (size_t i = 0; i < size; ++i)
        if (contents[i] != byte)
            return 0;
    return 1;
}

static void failures(void)
{
    int value = 23, n = -1;
    assert(sscanf("", "%d", &value) == EOF && value == 23);
    assert(sscanf(" \v", "%d", &value) == EOF && value == 23);
    assert(sscanf("q", "%d%n", &value, &n) == 0 && value == 23 && n == -1);
    assert(sscanf("", "y") == EOF && sscanf("x", "y") == 0);
    assert(sscanf("", "") == 0 && sscanf(" \f ", " \n ") == 0);
    assert(sscanf("", "%n%d", &n, &value) == 0 && n == 0 && value == 23);
    assert(sscanf("123", "%*d%d", &value) == 0 && value == 23);
    assert(sscanf("a", "a%d", &value) == EOF && value == 23);
    assert(sscanf("%", "%%%d", &value) == EOF && value == 23);
    assert(sscanf("+", "%d", &value) == 0 && value == 23);

    errno = 0;
    assert(sscanf("2147483648", "%d", &value) == EOF && errno == ERANGE && value == 23);
    signed char narrow = 23;
    errno = 0;
    assert(sscanf("-129", "%hhd", &narrow) == EOF && errno == ERANGE && narrow == 23);
    unsigned long long large = 23;
    errno = 0;
    assert(sscanf("2 18446744073709551616", "%d%llu", &value, &large) == 1 &&
           errno == ERANGE && value == 2 && large == 23);
    errno = 0;
    assert(sscanf("184467440737095516160 5", "%*u%d", &value) == 1 &&
           value == 5 && errno == 0);

    const char *invalid[] = { "%", "%0d", "%Ld", "%hs", "%[", "%[]",
                              "%[^]", "%*n", "%1n", "%2%", "%*%", "%m" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        errno = 0;
        assert(sscanf("1", invalid[i]) == EOF && errno == EINVAL);
    }
    errno = 0;
    assert(sscanf("1", "%18446744073709551616d", &value) == EOF && errno == EOVERFLOW);
    assert(sscanf("1", "%18446744073709551615d", &value) == 1 && value == 1);

    const char *unsupported[] = { "%a", "%lA", "%e", "%E", "%f", "%F", "%g", "%G" };
    /* Check storage byte-wise without compiler-specific soft-float helpers. */
    float f;
    double d;
    long double ld;
    memset(&f, 0, sizeof(f));
    memset(&d, 0, sizeof(d));
    memset(&ld, 0, sizeof(ld));
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(*unsupported); ++i) {
        errno = 0;
        int result = i == 1 ? sscanf("1", unsupported[i], &d)
                            : sscanf("1", unsupported[i], &f);
        assert(result == EOF && errno == ENOSYS &&
               filled_with(&f, sizeof(f), 0) && filled_with(&d, sizeof(d), 0));
    }
    errno = 0;
    assert(sscanf("1 2", "%d%Lf", &value, &ld) == 1 &&
           errno == ENOSYS && value == 1 && filled_with(&ld, sizeof(ld), 0));
    errno = 0;
    assert(sscanf("1", "%*f") == EOF && errno == ENOSYS);
}

static void stream(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const char source[] = "1: +x 0x. 078! 0x9; x\0\xffZ";
    assert(fwrite(source, 1, sizeof(source) - 1, file) == sizeof(source) - 1);
    rewind(file);

    int value, n = -1;
    assert(fscanf(file, "%d%n!", &value, &n) == 1 && value == 1 && n == 1);
    assert(ftell(file) == 1 && fgetc(file) == ':');
    assert(fscanf(file, "%d", &value) == 0 && value == 1 && fgetc(file) == 'x');
    assert(fscanf(file, "%i", &value) == 0 && value == 1 && fgetc(file) == '.');
    assert(fscanf(file, "%i", &value) == 1 && value == 7 && fgetc(file) == '8');
    assert(fgetc(file) == '!');
    assert(fscanf(file, "%2i", &value) == 0 && value == 7 && fgetc(file) == '9');
    assert(fgetc(file) == ';');
    assert(ungetc('4', file) == '4');
    assert(scan_file(file, "%d%n", &value, &n) == 1 && value == 4 && n == 1);
    assert(fgetc(file) == ' ');
    unsigned char bytes[3] = {0};
    assert(fscanf(file, "%3c", bytes) == 1 &&
           bytes[0] == 'x' && bytes[1] == 0 && bytes[2] == 0xff);
    assert(!feof(file));
    assert(fscanf(file, "%2c", bytes) == 0 &&
           bytes[0] == 'x' && bytes[1] == 0 && bytes[2] == 0xff);
    assert(feof(file) && !ferror(file));
    assert(fscanf(file, "%c", bytes) == EOF);
    rewind(file);
    float unsupported = 0;
    errno = 0;
    assert(fscanf(file, "%f", &unsupported) == EOF && errno == ENOSYS &&
           ftell(file) == 0 && fgetc(file) == '1');
    assert(fclose(file) == 0);

    file = tmpfile();
    assert(file != NULL && fputs("a\x80 b", file) != EOF);
    rewind(file);
    wchar_t wide[3] = { 'X', 'X', 'X' };
    errno = 0;
    assert(fscanf(file, "%ls", wide) == EOF && errno == EILSEQ &&
           wide[0] == 'X' && wide[1] == 'X' && wide[2] == 'X');
    assert(!feof(file) && !ferror(file) && ftell(file) == 2 && fgetc(file) == ' ');
    assert(fscanf(file, "%lc", wide) == 1 && wide[0] == 'b' && wide[1] == 'X');
    assert(fclose(file) == 0);

    file = fopen("/dev/null", "w");
    assert(file != NULL);
    errno = 0;
    assert(fscanf(file, "%d", &value) == EOF && ferror(file) &&
           !feof(file) && errno == EBADF);
    assert(fclose(file) == 0);
}

int main(void)
{
    integers();
    text_fields();
    failures();
    stream();

    /* The runner passes a pipe, so mismatches cannot seek back. */
    int value;
    char ch;
    assert(scanf("%d!", &value) == 1 && value == 18);
    assert(scan_stdin("%c %d", &ch, &value) == 2 && ch == '?' && value == 24);
    assert(getchar() == '\n' && scanf("%d", &value) == EOF && feof(stdin));
    return 0;
}
