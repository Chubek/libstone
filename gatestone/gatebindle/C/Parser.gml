parser {
    start expression;
    rule expression = expression PLUS term => Add
                    | expression MINUS term => Subtract
                    | term => Pass;
    rule term = term STAR unary => Multiply
              | term SLASH unary => Divide
              | term PERCENT unary => Remainder
              | unary => Pass;
    rule unary = MINUS unary => Negate
               | PLUS unary => Pass
               | primary => Pass;
    rule primary = INTEGER => Integer
                 | LPAREN expression RPAREN => Pass;
}
