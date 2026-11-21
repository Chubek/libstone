typedef unsigned long long ULL;

extern int controller(void);
extern int unselected(void);
static int global = 2;
static volatile int gate;
static int *static_pointer = &_Generic(0, int: global);
static int table[_Generic(0, int: 3, default: unselected())];

_Static_assert(_Generic(controller(), int: 1, default: unselected()), "only the selected arm is an ICE");
_Static_assert(_Generic(1UL, ULL: 0, unsigned long: 1), "equal width does not mean compatible");
_Static_assert(_Generic("x", char *: 1, default: 0), "array decays for matching");
_Static_assert(sizeof _Generic(1, int: "xyz", default: 0) == 4, "result retains array type");
_Static_assert(_Generic(controller, int (*)(void): 1), "function decays for matching");
_Static_assert(_Generic((signed char)1, signed char: 1, int: 0), "no integer promotion");
_Static_assert(_Generic(1, int: 1, default: 1/0), "do not evaluate unselected expressions");

static int f(int x) { return x + global; }

int main(void) {
    const int qualified = 0;
    const int *pointer = &qualified;
    int n = 3;
    int *p = &_Generic(qualified, int: n, default: global);
    if (p != &n || !_Generic(pointer, const int *: 1, int *: 0))
        return 1;
    _Generic(n++, int: *p, default: global) += 4;
    if (n != 7 || *static_pointer != 2)
        return 2;
    _Generic(n, int: (void)0, default: (void)unselected());
    table[2] = _Generic(n, int: f, default: f)(n);
    return _Generic(gate, int: table[2] != 9, default: 1);
}
