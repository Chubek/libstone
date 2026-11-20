static int limit = (int)0x1.fp3;
static _Bool off = (_Bool)0.0;

static int check(int value) {
    switch (value) {
    case (int)15.875:
        return (int)2.75f;
    default:
        return 0;
    }
}

int main(void) {
    int i = (int)0x1.fp3;
    _Bool on = (_Bool)0.25;
    int *null = (int)0.75;
    if (off || !on || null) return 1;
    if (i != 15 || limit != i || check(i) != 2) return 2;
    return 0;
}
