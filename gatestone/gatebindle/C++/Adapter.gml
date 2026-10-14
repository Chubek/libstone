adapter {
    lower Assign = assign;
    lower BitOr = bor;
    lower BitXor = bxor;
    lower BitAnd = band;
    lower Shl = shl;
    lower Shr = ashr;
    lower Add = add;
    lower Subtract = sub;
    lower Multiply = mul;
    lower Divide = sdiv;
    lower Remainder = srem;
    lower Negate = neg;
    lower BitNot = not;
    lower Call = call;
    lower CallEmpty = callempty;
    lower Integer = integer;
    lower Variable = variable;
    lower Pass = identity;
}
