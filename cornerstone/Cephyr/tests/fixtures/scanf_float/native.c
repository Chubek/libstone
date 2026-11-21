#include <stdio.h>

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
