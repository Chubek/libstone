#include "calc.h"

calc_expr_t make_number_in_other_unit(calc_arena *arena) {
    return calc_Number(arena, 42, calc_int_none());
}
