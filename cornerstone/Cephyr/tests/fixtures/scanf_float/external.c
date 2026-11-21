#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int cephyr_scan_direct(float *, double *, float *, double *, float *, double *,
                       float *, double *, int *);
int cephyr_scan_pipe(float *);
int cephyr_scan_pipe_tail(char *, double *);

int external_scan_stdin(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vscanf(format, args);
    va_end(args);
    return result;
}

static int scan_text(const char *text, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsscanf(text, format, args);
    va_end(args);
    return result;
}

static uint32_t float_bits(const float *value)
{
    uint32_t bits;
    memcpy(&bits, value, sizeof(bits));
    return bits;
}

static uint64_t double_bits(const double *value)
{
    uint64_t bits;
    memcpy(&bits, value, sizeof(bits));
    return bits;
}

static void values(void)
{
    float a, c, e, g;
    double b, d, f, h;
    int n = -1;
    assert(cephyr_scan_direct(&a, &b, &c, &d, &e, &f, &g, &h, &n) == 8);
    assert(float_bits(&a) == 0x3fc00000U && double_bits(&b) == 0x3fe0000000000000UL);
    assert(float_bits(&c) == 0x40000000U && double_bits(&d) == 0x8000000000000000UL);
    assert(float_bits(&e) == 0x3e800000U && double_bits(&f) == 0xfff0000000000000UL);
    assert(float_bits(&g) == 0x7fc00000U && double_bits(&h) == 0x4029000000000000UL);
    assert(n == (int)strlen("1.5 0X1p-1 0x2 -0 .25 -Inf +NaN() 1.25e1"));

    /* A decimal slightly above the binary32 midpoint rounds up for float
       but to the exact midpoint for double. */
    assert(scan_text("1.0000000596046447753906250001 1.0000000596046447753906250001",
                     "%f %lf", &a, &b) == 2);
    assert(float_bits(&a) == 0x3f800001U && double_bits(&b) == 0x3ff0000010000000UL);
    assert(scan_text("0x1p-149 0x1p-1074", "%f %lf", &a, &b) == 2);
    assert(float_bits(&a) == 1 && double_bits(&b) == 1);
    assert(scan_text("-nAn(PaYl0ad_) +iNfInItY", "%lf %F", &b, &a) == 2);
    assert(double_bits(&b) == 0xfff8000000000000UL && float_bits(&a) == 0x7f800000U);
}

static void width_and_failures(void)
{
    float f = 13;
    double d = 13;
    char tail = '?';
    int n = -1, i = -1;
    assert(sscanf(" \t-1.25", "%4f%n%c", &f, &n, &tail) == 2 &&
           float_bits(&f) == 0xbf99999aU && n == 6 && tail == '5');
    assert(sscanf("0x4", "%1f%c", &f, &tail) == 2 &&
           float_bits(&f) == 0 && tail == 'x');
    assert(sscanf("InfInItY", "%3f%c", &f, &tail) == 2 &&
           float_bits(&f) == 0x7f800000U && tail == 'I');
    assert(sscanf("1e+2", "%1lf%c", &d, &tail) == 2 &&
           double_bits(&d) == 0x3ff0000000000000UL && tail == 'e');
    assert(sscanf("infinity", "%4f", &f) == 0);
    assert(sscanf("0x1p+2", "%5lf", &d) == 0 && double_bits(&d) == 0x3ff0000000000000UL);
    assert(sscanf("nan(X)", "%5f", &f) == 0);

    const uint32_t unchanged = float_bits(&(float){13});
    f = 13;
    n = -1;
    assert(sscanf(" \t", "%f%n", &f, &n) == EOF && float_bits(&f) == unchanged && n == -1);
    const char *prefixes[] = {"+", ".", "i", "1e+", "0x.", "nan("};
    for (size_t j = 0; j < sizeof(prefixes) / sizeof(*prefixes); ++j)
        assert(sscanf(prefixes[j], "%f", &f) == 0 && float_bits(&f) == unchanged);
    assert(sscanf("2 1e+x", "%d %f%n", &i, &f, &n) == 1 &&
           i == 2 && float_bits(&f) == unchanged && n == -1);
    assert(sscanf("2 ", "%d %f", &i, &f) == 1 && i == 2 && float_bits(&f) == unchanged);
    assert(sscanf("2.", "%*f%lf", &d) == 0 && double_bits(&d) == 0x3ff0000000000000UL);
    assert(sscanf("", "%n%f", &n, &f) == 0 && n == 0 && float_bits(&f) == unchanged);

    errno = 0;
    assert(sscanf("1e9999 9", "%*f %d", &i) == 1 && i == 9 && errno == 0);
    n = -1;
    assert(sscanf("1e+!9", "%*f%n", &n) == 0 && n == -1);

    char text[1024];
    memset(text, '0', sizeof(text) - 5);
    memcpy(text + sizeof(text) - 5, "2.5!", 5);
    assert(sscanf(text, "%lf%n%c", &d, &n, &tail) == 2 &&
           double_bits(&d) == 0x4004000000000000UL && n == (int)sizeof(text) - 2 && tail == '!');
}

static int scan_file(FILE *file, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfscanf(file, format, args);
    va_end(args);
    return result;
}

static void streams(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    assert(fputs("100er 1e+! 0x.; infiX nan(foo-) +.x 0x1.p+? 2e+3 \xff 1.5, 4",
                 file) >= 0);
    rewind(file);
    float f = 13;
    double d = 13;
    const uint32_t unchanged_f = float_bits(&f);
    const uint64_t unchanged_d = double_bits(&d);
    int n = -1, i = -1;
    assert(fscanf(file, "%f", &f) == 0 && float_bits(&f) == unchanged_f && ftell(file) == 4 &&
           fgetc(file) == 'r');
    assert(fscanf(file, "%f", &f) == 0 && float_bits(&f) == unchanged_f && fgetc(file) == '!');
    assert(fscanf(file, "%lf", &d) == 0 && double_bits(&d) == unchanged_d && fgetc(file) == ';');
    assert(fscanf(file, "%lf", &d) == 0 && double_bits(&d) == unchanged_d && fgetc(file) == 'X');
    assert(fscanf(file, "%lf", &d) == 0 && double_bits(&d) == unchanged_d && fgetc(file) == '-');
    assert(fgetc(file) == ')');
    assert(fscanf(file, "%f", &f) == 0 && float_bits(&f) == unchanged_f && fgetc(file) == 'x');
    assert(fscanf(file, "%f", &f) == 0 && float_bits(&f) == unchanged_f && fgetc(file) == '?');
    assert(fscanf(file, "%3g", &f) == 0 && float_bits(&f) == unchanged_f && fgetc(file) == '3');
    assert(fscanf(file, "%f", &f) == 0 && float_bits(&f) == unchanged_f && fgetc(file) == 255);
    assert(ungetc('-', file) == '-');
    char ch;
    assert(scan_file(file, "%c %3lf%n", &ch, &d, &n) == 2 && ch == '-' &&
           double_bits(&d) == 0x3ff8000000000000UL && n == 5);
    assert(fgetc(file) == ',');
    assert(fscanf(file, "%d%f", &i, &f) == 1 && i == 4 && float_bits(&f) == unchanged_f && feof(file));
    assert(fscanf(file, "%f", &f) == EOF && float_bits(&f) == unchanged_f);
    assert(fclose(file) == 0);

    file = fopen("/dev/null", "w");
    assert(file != NULL);
    errno = 0;
    assert(fscanf(file, "%f", &f) == EOF && ferror(file) && !feof(file) &&
           errno == EBADF && float_bits(&f) == unchanged_f);
    assert(fclose(file) == 0);

    /* stdin is a pipe. An incomplete item consumes its prefix but leaves
       exactly one lookahead byte for the next directive. */
    assert(cephyr_scan_pipe(&f) == 0 && float_bits(&f) == unchanged_f);
    assert(cephyr_scan_pipe_tail(&ch, &d) == 2 && ch == '?' &&
           double_bits(&d) == 0x4020000000000000UL);
    assert(getchar() == '\n');
    assert(cephyr_scan_pipe(&f) == EOF && feof(stdin) && float_bits(&f) == unchanged_f);
}

int main(void)
{
    values();
    width_and_failures();
    streams();
    return 0;
}
