// Node-to-handler dispatch metadata consumed by gateml.VisitorSpec.
profile {
    option visitor_order = "postorder";
    option visitor_terminals = "false";
    option visit_Add = "add";
    option visit_Subtract = "sub";
    option visit_Multiply = "mul";
    option visit_Divide = "sdiv";
    option visit_Remainder = "srem";
    option visit_Negate = "neg";
    option visit_Integer = "integer";
    option visit_Pass = "identity";
}
