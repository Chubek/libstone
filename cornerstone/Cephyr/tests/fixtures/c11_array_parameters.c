/* C11 6.7.6.2p1/p4/p5 and 6.7.6.3p7/p12/p15. */
enum { MIN = 3 };
typedef int Row[MIN];

int bound(void);  /* Deliberately undefined: a prototype must not evaluate it. */
int pick(int a[bound()]);
int pick(int a[const restrict *]);
int pick(int *a);
int pick(int a[static const restrict MIN])
{
    _Static_assert(sizeof(a) == sizeof(int *), "array parameter is a pointer");
    _Static_assert(_Generic(&a, int *const restrict *: 1, default: 0),
                   "bracket qualifiers apply to the parameter pointer");
    a[0] += a[1];
    return a[0] + a[2];
}

int stride(int (*a)[MIN]);
int stride(int (a)[restrict static 2][MIN])
{
    _Static_assert(sizeof(*a) == sizeof(Row), "inner array survives adjustment");
    _Static_assert(_Generic(&a, Row *restrict *: 1, default: 0),
                   "restrict qualifies the pointer to the row");
    return a[1][2];
}

int typedef_adjust(const int *);
int typedef_adjust(const Row a)
{
    _Static_assert(_Generic(a, const int *: 1, default: 0),
                   "qualifiers on an array typedef apply to elements");
    return a[1];
}

int through_volatile_pointer(int a[volatile])
{
    _Static_assert(_Generic(&a, int *volatile *: 1, default: 0),
                   "bracket volatile qualifies the pointer");
    a = a + 1;
    return *a;
}

int bounds_in_scope(int n, int a[static n]);
int bounds_in_scope(int n, int a[static sizeof(n++)])
{
    return a[n - 1];
}

int previous_array(int a[], int b[static sizeof(a) / sizeof(a[0])])
{
    return a[0] + b[1];
}

int register_param(register int a[static 2])
{
    return a[0] + a[1];  /* The parameter is a register pointer, not a register array. */
}

/* [*] is at prototype scope in the callback declarator, not at block scope
   in this enclosing definition. */
int invoke(int n, int callback(int a[*]), int a[])
{
    return callback(a) + n;
}

int invoke_with_scope(int n, int callback(int a[n]), int a[])
{
    return callback(a) + n;
}

extern int site_bound;
typedef int Callback(int a[site_bound]);
int decl_scope(int site_bound[])
{
    Callback *p = pick;  /* The typedef's bound referred to the file-scope int. */
    return p(site_bound);
}

int abstract_param(int [const static 3]);
int matrix_prototype(int a[*][MIN]);
int matrix_prototype(int (*a)[MIN]);
struct Incomplete;
int incomplete_prototype(struct Incomplete);
int pointer_to_incomplete(struct Incomplete *a) { return a != 0; }

int main(void)
{
    int values[4] = {3, 4, 5, 6};
    int matrix[2][MIN] = {{8, 9, 10}, {11, 12, 13}};
    if (pick(values) != 12 || values[0] != 7) return 1;
    if (stride(matrix) != 13) return 2;
    if (typedef_adjust(values) != 4 || through_volatile_pointer(values) != 4) return 3;
    if (bounds_in_scope(4, values) != 6 || previous_array(values, values) != 11) return 4;
    if (invoke(1, pick, values) != 17 || invoke_with_scope(2, pick, values) != 22) return 5;
    if (decl_scope(values) != 24) return 6;
    if (register_param(values) != 23) return 7;
    return pointer_to_incomplete(0);
}
