parser {
    start expression;
    rule expression = INTEGER => Integer
                    | IDENT => Variable
                    | LPAREN PLUS expression expression RPAREN => Add
                    | LPAREN MINUS expression expression RPAREN => Subtract
                    | LPAREN STAR expression expression RPAREN => Multiply
                    | LPAREN QUOTIENT expression expression RPAREN => Divide
                    | LPAREN REMAINDER expression expression RPAREN => Remainder
                    | LPAREN MINUS expression RPAREN => Negate
                    | LPAREN IDENT expression RPAREN => NamedCall
                    | LPAREN IDENT RPAREN => NamedCallEmpty;
}
