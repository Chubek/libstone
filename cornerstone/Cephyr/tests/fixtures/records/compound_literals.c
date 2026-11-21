/* C11 6.5.2.5: file-scope unnamed objects and address constants. */
struct pair {
    int x, y;
};

union number {
    long value;
    int small;
};

static int *array = (int []) { [3] = 41, 47, };
static int *end = (int []) { 61, 67 } + 1;
static int *subobject = &(struct pair) { .y = 7, .x = 5 }.y;
static int *element = &(int []) { 71, 73 }[1];
typedef int scalar;
static int (*whole)[2] = &(int [2]) { 11, 13 };
static const struct pair *const constant = &(const struct pair) { .y = 37 };
static int **nested = &(int *) { &(int) { 53 } };
static int *separate = &(int) { 53 };
static char *text = (char []) { "abc" };

_Static_assert(sizeof (int []) { [3] = 1, 2 } == 5 * sizeof(int),
               "the type includes the largest designated or sequential slot");
_Static_assert(sizeof (struct pair) { .y = 1 }.y == sizeof(int),
               "the sizeof operand includes the postfix suffix");
_Static_assert(sizeof (int *) { &(int) { 3 } } == sizeof(int *),
               "unevaluated static literals may contain address constants");

static int plus_one(int n) {
    return n + 1;
}

#if defined(__riscv)
static int sum(struct pair arg) {
    return arg.x + arg.y;
}
#endif

static int repeat(void) {
    struct pair *last, *p = 0;
    int j = 0;
again:
    last = p;
    p = &(struct pair) { .x = j ? p->x + 1 : 10 };
    if (j++ < 1)
        goto again;
    return p == last && last->x == 11 && last->y == 0;
}

static int recursive(int n, int *outer) {
    int *p = &(int) { n };
    if (n)
        return recursive(n - 1, p) && *p == n;
    return p != outer && *outer == 1;
}

int main(void) {
    if (array[0] || array[3] != 41 || array[4] != 47 || *end != 67)
        return 1;
    if (*subobject != 7 || *element != 73 || (*whole)[1] != 13 ||
        constant->x || constant->y != 37)
        return 2;
    *subobject = 17;
    text[0] = 'z';
    if (*subobject != 17 || text[0] != 'z' || text[3] || **nested != 53 ||
        *separate != 53 || *nested == separate)
        return 3;

    int i = 8;
    static unsigned long size = sizeof (int) { i++ };
    if (size != sizeof(int) || i != 8)
        return 11;
    struct pair value = (struct pair) { .y = i, .x = 2 };
    if (value.x + value.y != 10 || (union number) { .value = i }.value != 8 ||
        (int [3]) { 4, [2] = i }[1] != 0)
        return 4;
#if defined(__riscv)
    if (sum((struct pair) { .y = i, .x = 2 }) != 10)
        return 9;
#endif
    if (++(scalar) { i } != 9 || (int) { i }++ != 8 || ((int) { 0 } = i) != i ||
        ((struct pair) { i }).x++ != 8)
        return 5;
    char *s = (char []) { "xy" };
    s[0] = 'w';
    if (s[0] != 'w' || s[1] != 'y' || s[2] ||
        sizeof (int [2]) { i++, 0 } != 2 * sizeof(int) || i != 8)
        return 6;
    if (_Generic(&(int []) { i++ }, int (*)[1]: 1, default: 0) != 1 || i != 8)
        return 7;
    volatile struct pair *v = &(volatile struct pair) { .y = i };
    if (v->x || v->y != 8 || (int (*)(int)) { plus_one }(3) != 4)
        return 10;
    {
        typedef short scalar;
        if (sizeof (scalar) { i } != sizeof(short))
            return 12;
    }
    if (!repeat() || !recursive(3, 0))
        return 8;
    return 0;
}
