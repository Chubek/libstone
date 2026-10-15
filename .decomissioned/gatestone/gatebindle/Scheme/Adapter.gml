adapter {
    lower Integer = integer;
    lower Variable = variable;
    lower Add = add;
    lower Subtract = sub;
    lower Multiply = mul;
    lower Divide = sdiv;
    lower Remainder = srem;
    lower Negate = neg;
    // Named calls expose IDENT directly, not a lowered receiver value.
    lower NamedCall = namedcall;
    lower NamedCallEmpty = namedcallempty;
}
