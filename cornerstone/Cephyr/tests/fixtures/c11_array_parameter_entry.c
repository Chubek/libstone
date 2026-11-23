/* The adjusted pointer keeps no outer dimension, but the function definition
   must still evaluate each nonconstant outer bound on entry (§6.9.1p10). */
static int calls;
static int extent(void) { ++calls; return 2; }

int prototype_only(void); /* Must not generate a call or even a link reference. */
static int pick(int, int [static prototype_only()][2]);
static int pick(int n, int a[const static (n++ ? extent() : 1)][2])
{
    _Static_assert(sizeof *a == 2 * sizeof(int), "constant inner stride");
    return n + a[1][1];
}

static int global_bound = 1;
static int shadow(int global_bound[global_bound++], int extent)
{
    return global_bound[0] + extent;
}

static int observe(int *p, int a[static ++*p], int b[1 + sizeof(++*p) / sizeof(int)])
{
    return *p + a[0] + b[1];
}

/* The last named parameter is valid even when it is ineligible for va_start. */
static int variadic(int a[extent()], ...) { return a[0]; }

int main(void)
{
    int rows[2][2] = {{3, 4}, {5, 6}};
    int count = 1;
    if (pick(1, rows) != 8 || calls != 1) return 1;
    if (pick(0, rows) != 7 || calls != 1) return 2;
    if (shadow(rows[0], 7) != 10 || global_bound != 2) return 3;
    if (observe(&count, rows[0], rows[1]) != 11 || count != 2) return 4;
    if (variadic(rows[0], 1) != 3 || calls != 2) return 5;
    return 0;
}
