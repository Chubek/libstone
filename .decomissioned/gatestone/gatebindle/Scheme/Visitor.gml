profile {
    option visitor_order = "postorder";
    option visitor_terminals = "false";
    option visit_Integer = "integer";
    option visit_Variable = "variable";
    option visit_Add = "add";
    option visit_Subtract = "sub";
    option visit_Multiply = "mul";
    option visit_Divide = "sdiv";
    option visit_Remainder = "srem";
    option visit_Negate = "neg";
    option visit_NamedCall = "namedcall";
    option visit_NamedCallEmpty = "namedcallempty";
}
