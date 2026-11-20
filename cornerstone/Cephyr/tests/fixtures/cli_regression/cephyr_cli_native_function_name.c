static const char *label(void) {
    static const char *const saved = __func__;
    return saved;
}

static int same(void) {
    static const char (*whole)[5] = &__func__;
    return *whole == __func__ && (*whole)[2] == 'm' && (*whole)[4] == 0;
}

static unsigned long size_only(void) {
    return sizeof __func__;
}

int main(void) {
    const char *name = label();
    if (name != label() || name[0] != 'l' || name[4] != 'l' || name[5]) return 1;
    if (!same() || size_only() != 10) return 2;
    if (sizeof __func__ != 5 || __func__[0] != 'm' ||
        __func__[3] != 'n' || __func__[4]) return 3;
    return 0;
}
