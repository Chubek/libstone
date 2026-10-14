adapter {
    lower Assign = assign;
    lower BitOr = bor;
    lower BitXor = bxor;
    lower BitAnd = band;
    lower Shl = shl;
    lower Shr = lshr;
    lower Add = add;
    lower Subtract = sub;
    lower Multiply = mul;
    lower RealDiv = sdiv;
    lower IntDiv = sdiv;
    lower Modulo = srem;
    lower Negate = neg;
    lower BitNot = not;
    lower Call = call;
    lower CallEmpty = callempty;
    lower Integer = integer;
    lower Variable = variable;
    lower Pass = identity;
}
