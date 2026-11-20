static int tick(int recur) {
    static int value = 7;
    static int *slot = &value;
    ++*slot;
    return recur ? tick(0) : value;
}

static int other_tick(void) {
    static int value;
    return ++value;
}

static int from_label(int jump) {
    if (jump)
        goto after_declaration;
    {
        static int table[][2] = {1, 3, 5};
        static int row[4] = {6, 7, 8};
        static int *item = &row[1];
        static void *self = &self;
after_declaration:
        if (self != &self || item != &row[1] || sizeof(table) != 16 ||
            table[1][0] != 5 || table[1][1]) return -1;
        table[0][0] += *item;
        return table[0][0];
    }
}

int main(void) {
    if (tick(1) != 9 || tick(0) != 10) return 1;
    if (other_tick() != 1 || other_tick() != 2) return 2;
    if (from_label(1) != 8 || from_label(0) != 15) return 3;
    return 0;
}
