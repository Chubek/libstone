struct Port {
    unsigned low : 8;
    volatile signed delta : 13;
    unsigned companion : 10;
    volatile _Bool ready : 1;
    volatile unsigned full : 32;
    unsigned : 0;
    unsigned tail : 3;
};

struct Wide {
    volatile unsigned span : 24;
    unsigned hold : 8;
};

void volatile_access_sequence(volatile struct Port *p) {
    p->delta;
    (void)_Generic(p->delta, int: 0, default: 1);
    (void)sizeof(+p->delta);
    p->delta = -5;
    p->delta += 1;
    p->delta++;
    p->ready = 7;
    p->full = 0x80000001u;
    (void)p->full;
    (void)p->tail;
    p->low;
}

void volatile_wide_sequence(struct Wide *p) {
    p->span;
    p->span = 0;
    p->span ^= 5;
}

static struct Port port[2] = {
    { .low = 0xa5, .delta = -3000, .companion = 0x151, .ready = 1, .tail = 7 },
    { .low = 0x11, .delta = 92, .companion = 0x22, .full = 0x01234567u },
};
static struct Wide wide = { .span = 0xfedcbau, .hold = 0xab };
static volatile struct NestedPort {
    unsigned marker;
    struct { unsigned value : 7; const unsigned limit : 4; };
} nested = { .marker = 0x43210u, .value = 125, .limit = 13 };

#ifndef __aarch64__
struct Split {
    unsigned char prefix;
    volatile unsigned field : 24;
    unsigned char suffix;
};
static struct Split split = { 0x71, 0xeeddccu, 0x97 };

void volatile_split_sequence(struct Split *p) {
    p->field;
    p->field += 0x102u;
}
#endif

int main(void) {
    volatile struct Port *p = port;
    int index = 0;
    if ((p[index++].delta += 3) != -2997 || index != 1 || port[1].delta != 92)
        return 21;
    if ((p->delta += (p->companion = 0x2ea, 2)) != -2995 || p->companion != 0x2ea)
        return 22;
    if ((p->delta = -4096) != -4096 || p->delta++ != -4096 || --p->delta != -4096)
        return 23;
    if ((p->ready = 0) || !(p->ready = 9) || !p->ready)
        return 24;
    if ((p->full = 0xffffffffu) != 0xffffffffu || ++p->full != 0 || p->full != 0)
        return 25;
    if (p->low != 0xa5 || p->companion != 0x2ea || p->tail != 7
        || port[1].low != 0x11 || port[1].companion != 0x22 || port[1].full != 0x01234567u)
        return 26;
    if (wide.span != 0xfedcbau || (wide.span ^= 0x00ff00) != 0xfe23bau || wide.hold != 0xab)
        return 27;
    if ((nested.value += 3) != 0 || nested.value || nested.limit != 13 || nested.marker != 0x43210u)
        return 28;
#ifndef __aarch64__
    if ((split.field += 0x102u) != 0xeedeceu || split.prefix != 0x71 || split.suffix != 0x97)
        return 29;
#endif
    return 0;
}
