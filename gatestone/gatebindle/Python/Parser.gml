parser {
    start expression;
    rule expression = IDENT ASSIGN expression => Assign
                    | bitor => Pass;
    rule bitor = bitor PIPE bitxor => BitOr
               | bitxor => Pass;
    rule bitxor = bitxor CARET bitand => BitXor
                | bitand => Pass;
    rule bitand = bitand AMP shift => BitAnd
                | shift => Pass;
    rule shift = shift SHL additive => Shl
               | shift SHR additive => Shr
               | additive => Pass;
    rule additive = additive PLUS multiplicative => Add
                  | additive MINUS multiplicative => Subtract
                  | multiplicative => Pass;
    rule multiplicative = multiplicative STAR unary => Multiply
                        | multiplicative SLASH unary => Divide
                        | multiplicative DSLASH unary => FloorDiv
                        | multiplicative PERCENT unary => Remainder
                        | unary => Pass;
    rule unary = MINUS unary => Negate
               | PLUS unary => Pass
               | TILDE unary => BitNot
               | power => Pass;
    rule power = postfix STARSTAR unary => Power
               | postfix => Pass;
    rule postfix = postfix LPAREN expression RPAREN => Call
                 | postfix LPAREN RPAREN => CallEmpty
                 | primary => Pass;
    rule primary = INTEGER => Integer
                 | IDENT => Variable
                 | LPAREN expression RPAREN => Pass;
}
