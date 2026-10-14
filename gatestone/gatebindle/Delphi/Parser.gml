parser {
    start expression;
    rule expression = IDENT ASSIGN expression => Assign
                    | bitor => Pass;
    rule bitor = bitor KW_OR bitxor => BitOr
               | bitxor => Pass;
    rule bitxor = bitxor KW_XOR bitand => BitXor
                | bitand => Pass;
    rule bitand = bitand KW_AND shift => BitAnd
                | shift => Pass;
    rule shift = shift SHL additive => Shl
               | shift SHR additive => Shr
               | additive => Pass;
    rule additive = additive PLUS multiplicative => Add
                  | additive MINUS multiplicative => Subtract
                  | multiplicative => Pass;
    rule multiplicative = multiplicative STAR unary => Multiply
                        | multiplicative SLASH unary => RealDiv
                        | multiplicative INTDIV unary => IntDiv
                        | multiplicative MODULO unary => Modulo
                        | unary => Pass;
    rule unary = MINUS unary => Negate
               | PLUS unary => Pass
               | KW_NOT unary => BitNot
               | postfix => Pass;
    rule postfix = postfix LPAREN expression RPAREN => Call
                 | postfix LPAREN RPAREN => CallEmpty
                 | primary => Pass;
    rule primary = INTEGER => Integer
                 | IDENT => Variable
                 | LPAREN expression RPAREN => Pass;
}
