static const short table[][3] = {1, 2, 3, {4}, 5, 6};
static const char names[][4] = {"abcd", {"ef"}, ""};
static int anchor;
static int *const pointer_rows[][2] = {&anchor, 0, {&anchor}};
static int counter;

static int next(void) {
    return ++counter;
}

int main(void) {
    short local[][2][2] = {{{next()}, {next()}}, next(), next(), next()};
    char rows[3][3] = {{"xy"}, "abc"};
    int *items[2][2] = {&anchor, {0}, &anchor};
    char large[32769] = {'a'};
    int *wide[2][24] = {{&anchor}, {&anchor}};

    if (sizeof(table) != 18 || table[0][2] != 3 || table[1][0] != 4 ||
        table[1][1] || table[2][1] != 6 || table[2][2]) return 1;
    if (sizeof(names) != 12 || names[0][3] != 'd' || names[1][1] != 'f' ||
        names[1][2] || names[2][3]) return 2;
    if (pointer_rows[0][0] != &anchor || pointer_rows[0][1] ||
        pointer_rows[1][0] != &anchor || pointer_rows[1][1]) return 3;
    if (sizeof(local) != 16 || counter != 5 || !local[0][0][0] ||
        !local[0][1][0] || !local[1][0][0] || !local[1][0][1] ||
        !local[1][1][0] || local[0][0][1] || local[0][1][1] || local[1][1][1]) return 4;
    if (rows[0][1] != 'y' || rows[0][2] || rows[1][2] != 'c' || rows[2][1]) return 5;
    if (items[0][0] != &anchor || items[0][1] ||
        items[1][0] != &anchor || items[1][1]) return 6;
    if (large[0] != 'a' || large[16384] || large[32768]) return 7;
    if (wide[0][0] != &anchor || wide[0][23] ||
        wide[1][0] != &anchor || wide[1][23]) return 8;
    return 0;
}
