#include <stdio.h>
#include <wchar.h>

#if defined(__x86_64__)
/* A variadic pointer is a GP argument; no long double return or by-value
   argument is needed. Exercise a Cephyr-owned object and its actual size. */
int cephyr_scan_extended(void)
{
    long double v = 0.L;
    int n = -1;
    if (sscanf("1.0000000000000000001!", "%Lf%n", &v, &n) != 1 ||
        v != 0x1.0000000000000002p0L || n != 21)
        return 0;
    return swscanf(L"-0x1p12000!", L"%La%n", &v, &n) == 1 &&
           v == -0x1p12000L && n == 10;
}
#endif

int external_scan_stdin(const char *, ...);

int cephyr_scan_direct(float *a, double *b, float *c, double *d,
                       float *e, double *f, float *g, double *h, int *n)
{
    /* Use every specifier, with strtod grammar regardless of the letter.
       Pointer arguments spill out of the variadic GP registers. */
    return sscanf("1.5 0X1p-1 0x2 -0 .25 -Inf +NaN() 1.25e1!",
                  "%a %lA %e %lE %f %lF %g %lG%n",
                  a, b, c, d, e, f, g, h, n);
}

int cephyr_scan_pipe(float *out)
{
    return scanf("%f", out);
}

int cephyr_scan_pipe_tail(char *tail, double *out)
{
    return external_scan_stdin("%c %lf", tail, out);
}
