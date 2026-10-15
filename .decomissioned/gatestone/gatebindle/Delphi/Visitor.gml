profile {
    option visitor_order = "postorder";
    option visitor_terminals = "false";
    option visit_Assign = "assign";
    option visit_BitOr = "bor";
    option visit_BitXor = "bxor";
    option visit_BitAnd = "band";
    option visit_Shl = "shl";
    option visit_Shr = "lshr";
    option visit_Add = "add";
    option visit_Subtract = "sub";
    option visit_Multiply = "mul";
    option visit_RealDiv = "sdiv";
    option visit_IntDiv = "sdiv";
    option visit_Modulo = "srem";
    option visit_Negate = "neg";
    option visit_BitNot = "not";
    option visit_Call = "call";
    option visit_CallEmpty = "callempty";
    option visit_Integer = "integer";
    option visit_Variable = "variable";
    option visit_Pass = "identity";
}
