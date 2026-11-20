static char large[65535];
static char table[][3] = {"ab", "cd"};
static long argument_value(int);  /* No definition: sizeof must not reference it. */
extern long external_object;
extern char incomplete[];

_Static_assert(sizeof large == 65535, "complete global array");
_Static_assert(sizeof table == 6 && sizeof table[0] == 3, "array and row");
_Static_assert(sizeof argument_value(12) == sizeof(long), "unevaluated call");
_Static_assert(sizeof external_object == 8, "unevaluated extern");
_Static_assert(sizeof *(1 ? &incomplete : &large) == 65535, "composite array type");
_Static_assert(1 || sizeof(1 / 0), "type checking does not evaluate division");
_Static_assert(sizeof((int)1.0e100) == 4, "type checking does not convert the value");

typedef char global_array[sizeof table];
static unsigned global_size = sizeof table + sizeof(table[1][0]);
static char derived[sizeof table / sizeof table[0]];
static unsigned self_sized[1] = {sizeof self_sized};

static unsigned adjusted(short value, char row[sizeof value], short (*matrix)[sizeof row]) {
    _Static_assert(sizeof value == 2, "named scalar parameter");
    _Static_assert(sizeof row == 8, "array parameter is adjusted");
    _Static_assert(sizeof *matrix == 16, "bound resolved in parameter scope");
    return sizeof *matrix;
}

static int check(void) {
    char table[7];
    typedef char local_array[sizeof table];
    global_array file_bound;
    _Static_assert(sizeof file_bound == 6, "typedef uses its definition scope");
    _Static_assert(sizeof __func__ == 6, "__func__ in an ICE");
    char by_name[sizeof __func__];
    static unsigned answer = (sizeof __func__ == 6) ? sizeof by_name : 0;
    static char *after_derived = &derived[sizeof derived];
    volatile short side_effect;
    register short reg;
    int value = 11;

    _Static_assert(sizeof(++side_effect) == 2, "volatile operand is unevaluated");
    _Static_assert(sizeof reg == 2, "register operand is unevaluated");
    _Static_assert(sizeof(value++, table) == 8, "comma converts array to pointer");
    _Static_assert(sizeof *(&table) == 7, "indirection preserves array type");
    if (sizeof(value += 3) != 4 || value != 11) return 1;
    {
        char table[2];
        local_array inherited;
        _Static_assert(sizeof inherited == 7 && sizeof table == 2, "nested scope");
    }
    switch (answer) {
    case sizeof by_name:
        return after_derived != derived + 2;
    default:
        return 2;
    }
}

int main(void) {
    short matrix[1][8];
    if (global_size != 7 || sizeof derived != 2 || self_sized[0] != 4) return 1;
    if (adjusted(0, table[1], matrix) != 16) return 2;
    if (check()) return 3;
    return 0;
}
