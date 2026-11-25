#include <stdarg.h>

static int calls;

static int bound(int n) {
    ++calls;
    return n;
}

/* Odd number of stack-passed words on all three ABIs. */
static int nine(int a, int b, int c, int d, int e, int f, int g, int h, int *p) {
    return a + b + c + d + e + f + g + h + *p;
}

/* Exercise anonymous register save areas and the incoming stack cursor while
   the dynamic frame is live. */
static int variadic(int n, ...) {
    int a[n];
    va_list ap;
    int sum = 0;
    a[0] = n;
    va_start(ap, n);
    for (int i = 0; i < n; ++i)
        sum += va_arg(ap, int);
    va_end(ap);
    return sum + a[0];
}

static long double wide_call(long double value) {
    return value + 1.0L;
}

static long double wide(int n, long double left, long double right) {
    long double a[n];
    a[n-1] = left + right;
    return wide_call(a[n-1]);
}

static int shape(int n) {
    int a[bound(2)][n++];
    int b[3][bound(n)];
    _Alignas(16) int c[bound(n)];
    if (calls != 3 || n != 5 || ((unsigned long)c & 15))
        return 1;
    n = 99;
    a[0][0] = 11;
    a[1][3] = 13;
    b[2][4] = 17;
    c[4] = 19;
    int i = 0;
    if (sizeof a != 32 || sizeof b != 60 || sizeof c != 20 ||
        !_Generic(sizeof a, unsigned long: 1, default: 0))
        return 2;
    if (sizeof a[i++] != 16 || i != 1 || sizeof a[0][i++] != 4 || i != 1)
        return 3;
    if (&a[1] - &a[0] != 1 || (a + 1)[0][3] != 13)
        return 4;
    if (nine(1, 2, 3, 4, 5, 6, 7, 8, &b[2][4]) != 53 ||
        a[0][0] != 11 || b[2][4] != 17 || c[4] != 19)
        return 5;
    if (sizeof(int[bound(3)][bound(4)]) != 48 || calls != 5)
        return 6;
    _Static_assert(_Alignof(int[n]) == _Alignof(int), "alignment is constant");
    if (_Alignof(int[bound(1)]) != _Alignof(int) || calls != 5)
        return 7;
    if (sizeof *((&a) + 0) != 32 || sizeof b[1] != 20)
        return 8;
    return 0;
}

static int lifetime(int extent) {
    int sum = 0;
    int retain[extent];
    retain[extent-1] = 47;
    /* More than a typical process stack if any loop exit leaks allocations. */
    for (int i = 0; i < 900; ++i) {
        char a[extent];
        a[extent-1] = 11;
        if (i % 3 == 0)
            continue;
        {
            char b[extent];
            b[extent-1] = 7;
            sum += b[extent-1];
            if (i % 3 == 1)
                goto out;
        }
        sum += a[extent-1];
    out:;
    }
    int i = 0;
    {
    again:;
        /* A jump backward exits this declaration's scope even though it
           remains within this compound statement. */
        char c[extent];
        c[extent-1] = 23;
        if (++i != 500) {
            sum += c[extent-1];
            goto again;
        }
    }
    for (char d[extent]; i > 0; --i) {
        d[extent-1] = 2;
        switch (i) {
        case 500: {
            char e[extent];
            e[extent-1] = 3;
            sum += e[extent-1];
            break;
        }
        default:
            if (i != 1)
                continue;
            break;
        }
        sum += d[extent-1];
        if (i == 1)
            break;
    }
    return retain[extent-1] + sum;
}

int main(void) {
    const int result = shape(4);
    if (result)
        return result;
    if (variadic(9, 1, 2, 3, 4, 5, 6, 7, 8, 9) != 54)
        return 20;
    if (lifetime(65536) != 47 + 600*7 + 300*11 + 499*23 + 3 + 2*2)
        return 21;
    if (wide(5, 4.0L, 5.0L) != 10.0L)
        return 22;
    return 0;
}
