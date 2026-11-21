struct S {
    _Static_assert(sizeof(int) == 4, "leading assertion");
    enum { n = 3 } count;
    _Static_assert(n == 3, "preceding declaration visible");
    int value[n];
    _Static_assert(_Alignof(int) == 4, "trailing assertion");
};

union U {
    _Static_assert(sizeof(struct S) == 16, "completed structure visible");
    struct S item;
    int i;
};

struct Packet {
    int length;
    char payload[];
    _Static_assert(sizeof(char) == 1, "assertions are not members");
};

_Static_assert(sizeof(union U) == 16, "assertions do not change layout");
_Static_assert(sizeof(struct Packet) == 4, "flexible member remains last");

#if defined(FAIL_ASSERTION)
struct Rejected { int value; _Static_assert(0, "record assertion diagnostic"); };
#endif

int main(void) {
    union U value = { .item = { .value = { 3, 2, 1 } } };
    return value.item.value[2] != 1;
}
